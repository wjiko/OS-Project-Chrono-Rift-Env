#include <SFML/Graphics.hpp>
#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <cstring>
#include <fstream>
#include "../shared.h"

GameState* global_state = nullptr;
volatile bool running = true;

// Helper to write to log file
void write_input_log(const std::string& msg) {
    std::cout << msg << std::endl;
    std::ofstream out("game_events.log", std::ios::app);
    if (out.is_open()) {
        out << msg << std::endl;
    }
}

// Local spinlock protects pending_action shared between main thread and player threads
volatile int local_input_lock = 0;
int pending_action = -1;
int pending_target = -1;
WeaponType pending_action_weapon = WPN_NONE;

void handle_stun(int) {
    if (global_state) {
        custom_lock_acquire(&global_state->custom_global_lock);
        global_state->players[0].is_stunned = true;
        custom_lock_release(&global_state->custom_global_lock);
        sleep(3);
        custom_lock_acquire(&global_state->custom_global_lock);
        global_state->players[0].is_stunned = false;
        custom_lock_release(&global_state->custom_global_lock);
    }
}

void handle_sigterm(int) {
    running = false;
    if (global_state) {
        // Kill the other two processes so they don't become orphans
        if (global_state->asp_pid > 0)     kill(global_state->asp_pid,     SIGTERM);
        if (global_state->arbiter_pid > 0) kill(global_state->arbiter_pid, SIGTERM);
        custom_lock_acquire(&global_state->custom_global_lock);
        global_state->game_running = false;
        custom_lock_release(&global_state->custom_global_lock);
    }
}

// One pthread per player — waits for its turn, reads pending_action, submits to Arbiter
void* player_thread(void* arg) {
    int pid = *(int*)arg;
    while (running) {
        custom_lock_acquire(&global_state->custom_global_lock);
        if (!global_state->game_running) {
            custom_lock_release(&global_state->custom_global_lock);
            break;
        }
        if (global_state->players[pid].pid == 0)
            global_state->players[pid].pid = getpid();

        bool my_turn = (global_state->current_turn_id == pid &&
                        global_state->current_turn_is_player &&
                        !global_state->action_submitted);
        custom_lock_release(&global_state->custom_global_lock);

        if (my_turn) {
            // Busy-wait for keypress (custom spinlock, no condition var)
            while (running) {
                custom_lock_acquire(&local_input_lock);
                if (pending_action != -1) {
                    int a = pending_action, t = pending_target;
                    WeaponType w = pending_action_weapon;
                    pending_action = -1;
                    pending_action_weapon = WPN_NONE;
                    custom_lock_release(&local_input_lock);

                    custom_lock_acquire(&global_state->custom_global_lock);
                    global_state->action_type      = a;
                    global_state->action_target_id = t;
                    global_state->action_weapon    = w;
                    global_state->action_submitted = true;
                    custom_lock_release(&global_state->custom_global_lock);
                    break;
                }
                custom_lock_release(&local_input_lock);
                usleep(30000);
            }
        } else {
            usleep(50000);
        }
    }
    return NULL;
}

// Draw a filled rectangle
static void drawRect(sf::RenderWindow& w, float x, float y, float W, float H, sf::Color c) {
    sf::RectangleShape r(sf::Vector2f(W, H));
    r.setPosition(x, y); r.setFillColor(c);
    w.draw(r);
}

// Draw outlined rectangle
static void drawBox(sf::RenderWindow& w, float x, float y, float W, float H,
                    sf::Color fill, sf::Color outline, float thick = 1.f) {
    sf::RectangleShape r(sf::Vector2f(W, H));
    r.setPosition(x, y);
    r.setFillColor(fill);
    r.setOutlineColor(outline);
    r.setOutlineThickness(thick);
    w.draw(r);
}

// Draw text
static void drawText(sf::RenderWindow& w, const sf::Font& f, const std::string& s,
                     float x, float y, unsigned sz, sf::Color c) {
    sf::Text t(s, f, sz);
    t.setFillColor(c);
    t.setPosition(x, y);
    w.draw(t);
}

// Filled bar (e.g. HP / stamina)
static void drawBar(sf::RenderWindow& w, float x, float y, float W, float H,
                    int val, int maxv, sf::Color bg, sf::Color fg) {
    drawRect(w, x, y, W, H, bg);
    float pct = (maxv > 0) ? std::max(0.f, std::min(1.f, (float)val / maxv)) : 0.f;
    if (pct > 0) drawRect(w, x, y, W * pct, H, fg);
}

int main() {
    signal(SIGUSR1, handle_stun);
    signal(SIGINT,  handle_sigterm);
    signal(SIGTERM, handle_sigterm);

    // Connect to Arbiter's shared memory
    int shm_fd = -1;
    for (int i = 0; i < 10 && shm_fd == -1; i++) {
        shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (shm_fd == -1) { std::cout << "HIP: waiting for Arbiter...\n"; sleep(1); }
    }
    if (shm_fd == -1) { perror("shm_open"); return 1; }

    global_state = (GameState*)mmap(0, sizeof(GameState),
                                    PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (global_state == MAP_FAILED) { perror("mmap"); return 1; }

    // SFML window — all rendering on this (main) thread
    sf::RenderWindow window(sf::VideoMode(900, 750), "Chrono Rift");
    window.setFramerateLimit(30);

    sf::Font font;
    bool has_font = font.loadFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    if (!has_font)
        has_font = font.loadFromFile("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");

    pthread_t player_threads[MAX_PLAYERS];
    int thread_ids[MAX_PLAYERS];
    bool threads_started = false;

    // Colours
    sf::Color BG(18,18,28), PANEL(28,30,46), BORDER(60,70,120);
    sf::Color HP_BG(80,20,20), HP_FG(70,190,70);
    sf::Color ST_BG(35,35,55), ST_FG(210,170,50);
    sf::Color C_PLAYER(120,200,255), C_ENEMY(255,100,100);
    sf::Color C_ACTIVE(255,235,60), C_DEAD(70,70,70), C_LOG(190,225,190);

    while (window.isOpen() && running) {
        // ── Event polling ──────────────────────────────────────────────────
        sf::Event ev;
        while (window.pollEvent(ev)) {
            if (ev.type == sf::Event::Closed) { handle_sigterm(0); window.close(); break; }

            if (ev.type == sf::Event::KeyPressed) {
                if (ev.key.code == sf::Keyboard::Escape) { handle_sigterm(0); window.close(); break; }

                // Read current state (no lock needed for quick local snapshot)
                bool setup_done, game_active, is_p_turn;
                int cur_id, n_en;
                custom_lock_acquire(&global_state->custom_global_lock);
                setup_done  = global_state->setup_complete;
                game_active = global_state->game_running;
                is_p_turn   = global_state->current_turn_is_player;
                cur_id      = global_state->current_turn_id;
                n_en        = global_state->num_enemies;
                custom_lock_release(&global_state->custom_global_lock);

                if (!setup_done) {
                    // Setup screen: press 1-4 to pick party size
                    int num = -1;
                    if (ev.key.code == sf::Keyboard::Num1) num = 1;
                    else if (ev.key.code == sf::Keyboard::Num2) num = 2;
                    else if (ev.key.code == sf::Keyboard::Num3) num = 3;
                    else if (ev.key.code == sf::Keyboard::Num4) num = 4;
                    if (num != -1) {
                        custom_lock_acquire(&global_state->custom_global_lock);
                        global_state->num_players    = num;
                        global_state->setup_complete = true;
                        custom_lock_release(&global_state->custom_global_lock);
                    }
                } else if (game_active && is_p_turn && cur_id >= 0) {
                    // Also read weapon_drop_pending while we have context
                    bool drop_pending;
                    WeaponType drop_type;
                    custom_lock_acquire(&global_state->custom_global_lock);
                    drop_pending = global_state->weapon_drop_pending;
                    drop_type    = global_state->weapon_drop_type;
                    custom_lock_release(&global_state->custom_global_lock);

                    custom_lock_acquire(&local_input_lock);
                    if (pending_action == -1) {
                        bool acted = false;
                        std::string log_msg = "[USER INPUT] Player " + std::to_string(cur_id) + " pressed ";

                        if (drop_pending) {
                            // Weapon drop Y/N mode — only accept Y or N
                            if (ev.key.code == sf::Keyboard::Y) {
                                pending_action = ACTION_PICKUP_WEAPON;
                                log_msg += "'Y' to PICK UP " + std::string(get_weapon_name(drop_type)) + "."; acted = true;
                            } else if (ev.key.code == sf::Keyboard::N) {
                                pending_action = ACTION_DECLINE_WEAPON;
                                log_msg += "'N' to DECLINE weapon drop."; acted = true;
                            }
                        } else {
                            // Normal action keys
                            if (ev.key.code >= sf::Keyboard::Num0 && ev.key.code <= sf::Keyboard::Num9) {
                                int tgt = ev.key.code - sf::Keyboard::Num0;
                                custom_lock_acquire(&global_state->custom_global_lock);
                                bool ok = (tgt < n_en && global_state->enemies[tgt].is_alive);
                                custom_lock_release(&global_state->custom_global_lock);
                                if (ok) {
                                    pending_action = ACTION_STRIKE; pending_target = tgt;
                                    log_msg += std::to_string(tgt) + " to STRIKE Enemy " + std::to_string(tgt); acted = true;
                                }
                            } else if (ev.key.code == sf::Keyboard::H) { pending_action = ACTION_HEAL; log_msg += "'H' to HEAL."; acted = true; }
                            else if (ev.key.code == sf::Keyboard::S)   { pending_action = ACTION_SKIP; log_msg += "'S' to SKIP."; acted = true; }
                            else if (ev.key.code == sf::Keyboard::E)   { pending_action = ACTION_EXHAUST; pending_target = 0; log_msg += "'E' to EXHAUST."; acted = true; }
                            else if (ev.key.code == sf::Keyboard::U)   { pending_action = ACTION_ULTIMATE; log_msg += "'U' ULTIMATE."; acted = true; }
                            else if (ev.key.code == sf::Keyboard::I)   { pending_action = ACTION_SWAP_IN; log_msg += "'I' SWAP IN."; acted = true; }
                            else if (ev.key.code == sf::Keyboard::L) {
                                pending_action = ACTION_LOCK_ARTIFACT;
                                pending_action_weapon = WPN_SOLAR_CORE;
                                log_msg += "'L' LOCK Solar Core."; acted = true;
                            } else if (ev.key.code == sf::Keyboard::K) {
                                pending_action = ACTION_LOCK_ARTIFACT;
                                pending_action_weapon = WPN_LUNAR_BLADE;
                                log_msg += "'K' LOCK Lunar Blade."; acted = true;
                            } else if (ev.key.code == sf::Keyboard::W) {
                                pending_action = ACTION_USE_WEAPON; pending_target = 0;
                                custom_lock_acquire(&global_state->custom_global_lock);
                                WeaponType wpn = WPN_NONE;
                                for (int s = 0; s < 20 && wpn == WPN_NONE; s++)
                                    wpn = global_state->players[cur_id].inv.slots[s];
                                pending_action_weapon = wpn;
                                custom_lock_release(&global_state->custom_global_lock);
                                log_msg += "'W' USE WEAPON (" + std::string(get_weapon_name(pending_action_weapon)) + ")."; acted = true;
                            }
                        }

                        if (acted) write_input_log(log_msg);
                    }
                    custom_lock_release(&local_input_lock);
                }
            }
        }

        // ── Spawn player threads once game starts ──────────────────────────
        if (!threads_started) {
            custom_lock_acquire(&global_state->custom_global_lock);
            bool go = global_state->game_running;
            int  np = global_state->num_players;
            custom_lock_release(&global_state->custom_global_lock);
            if (go) {
                for (int i = 0; i < np; i++) {
                    thread_ids[i] = i;
                    pthread_create(&player_threads[i], NULL, player_thread, &thread_ids[i]);
                }
                threads_started = true;
            }
        }

        // ── Snapshot game state for rendering ─────────────────────────────
        GameState snap;
        custom_lock_acquire(&global_state->custom_global_lock);
        snap = *global_state;
        custom_lock_release(&global_state->custom_global_lock);

        // ── Render ────────────────────────────────────────────────────────
        window.clear(BG);

        if (!snap.setup_complete) {
            // Setup screen
            if (has_font) {
                drawText(window, font, "CHRONO RIFT", 300, 240, 52, sf::Color(200,160,255));
                drawText(window, font, "Multi-Process Tactical RPG  |  OS Project Spring 2026", 205, 302, 16, sf::Color(150,150,200));
                drawText(window, font, "Select Party Size:", 340, 370, 22, sf::Color::White);
                drawText(window, font, "Press  1    2    3    or    4", 285, 408, 22, C_ACTIVE);
                drawText(window, font, "Roll Numbers: 24I-0523 / 24I-0822", 305, 470, 14, sf::Color(100,100,160));
            }
        } else {
            // Log bar
            drawRect(window, 15, 5, 870, 32, sf::Color(15,25,15));
            if (has_font) {
                drawText(window, font, snap.log_message, 20, 10, 14, C_LOG);
                char kb[48];
                snprintf(kb, sizeof(kb), "Kills %d/%d", snap.total_kills, snap.win_kill_target);
                drawText(window, font, kb, 780, 10, 14, C_ACTIVE);
            }

            // ── Players ───────────────────────────────────────────────────
            float py = 46;
            if (has_font) drawText(window, font, "PLAYERS", 15, py, 15, C_PLAYER);
            py += 22;

            for (int i = 0; i < snap.num_players; i++) {
                const Entity& p = snap.players[i];
                bool active = snap.current_turn_is_player && snap.current_turn_id == i;
                bool dead   = !p.is_alive;

                drawBox(window, 15, py, 400, 80, PANEL,
                        active ? C_ACTIVE : BORDER, active ? 2.f : 1.f);

                if (has_font) {
                    char nb[48];
                    snprintf(nb, sizeof(nb), "Player %d%s%s", i,
                             dead ? " [DEAD]" : "", p.is_stunned ? " [STUNNED]" : "");
                    drawText(window, font, nb, 23, py+5, 13,
                             dead ? C_DEAD : (active ? C_ACTIVE : C_PLAYER));

                    // HP
                    drawBar(window, 23, py+24, 200, 11, p.hp, p.max_hp, HP_BG, HP_FG);
                    char hb[32]; snprintf(hb, sizeof(hb), "HP %d/%d", p.hp, p.max_hp);
                    drawText(window, font, hb, 230, py+22, 11, sf::Color::White);

                    // Stamina
                    int disp_st_p = std::min(p.stamina, p.max_stamina);
                    drawBar(window, 23, py+41, 200, 10, p.stamina, p.max_stamina, ST_BG, ST_FG);
                    char sb[32]; snprintf(sb, sizeof(sb), "ST %d/%d", disp_st_p, p.max_stamina);
                    drawText(window, font, sb, 230, py+39, 11, sf::Color(200,200,90));

                    // Weapon
                    const char* wpn = "None";
                    for (int s = 0; s < 20; s++) if (p.inv.slots[s] != WPN_NONE) { wpn = get_weapon_name(p.inv.slots[s]); break; }
                    char wb[64]; snprintf(wb, sizeof(wb), "[%s]  LTS:%d", wpn, p.inv.lts_count);
                    drawText(window, font, wb, 23, py+57, 11, sf::Color(140,190,140));
                    if (active && !dead) {
                        if (snap.weapon_drop_pending) {
                            // Weapon drop mode — show Y/N prompt
                            drawRect(window, 15, py+68, 400, 28, sf::Color(60,40,10));
                            char dpbuf[80];
                            snprintf(dpbuf, sizeof(dpbuf), "Pick up %s?  [ Y ] Yes   [ N ] No",
                                     get_weapon_name(snap.weapon_drop_type));
                            drawText(window, font, dpbuf, 23, py+73, 12, sf::Color(255,220,80));
                        }
                    }
                }
                py += 90;
            }

            // ── Controls Legend ───────────────────────────────────────────
            float cy = 430;
            drawBox(window, 15, cy, 400, 305, PANEL, BORDER, 1.f);
            if (has_font) {
                drawText(window, font, "HOW TO PLAY / CONTROLS", 25, cy+10, 16, C_ACTIVE);
                
                drawText(window, font, "Wait for your yellow ST bar to fill up.", 25, cy+38, 12, sf::Color(200,200,200));
                drawText(window, font, "When highlighted yellow, press a key:", 25, cy+55, 12, sf::Color(200,200,200));
                
                cy += 82;
                drawText(window, font, "[ 0 - 9 ]   Strike a specific Enemy number.", 25, cy, 13, sf::Color::White); cy+=24;
                drawText(window, font, "[   W   ]   Use your equipped Weapon.", 25, cy, 13, sf::Color::White); cy+=24;
                drawText(window, font, "[   H   ]   Heal yourself (10% max HP).", 25, cy, 13, sf::Color::White); cy+=24;
                drawText(window, font, "[   S   ]   Skip turn (keeps 50% stamina).", 25, cy, 13, sf::Color::White); cy+=24;
                drawText(window, font, "[   E   ]   Exhaust: drain enemy's stamina.", 25, cy, 13, sf::Color::White); cy+=24;
                drawText(window, font, "[   I   ]   Swap in a weapon from storage.", 25, cy, 13, sf::Color::White); cy+=24;
                drawText(window, font, "[   L   ]   Lock Solar Core artifact.", 25, cy, 13, sf::Color::White); cy+=24;
                drawText(window, font, "[   K   ]   Lock Lunar Blade artifact.", 25, cy, 13, sf::Color::White); cy+=24;
                drawText(window, font, "[   U   ]   Ultimate: freeze enemies (10s).", 25, cy, 13, sf::Color::White); cy+=24;
            }

            // ── Enemies ───────────────────────────────────────────────────
            float ex = 440, ey = 46;
            if (has_font) drawText(window, font, "ENEMIES", ex, ey, 15, C_ENEMY);
            ey += 22;

            for (int i = 0; i < snap.num_enemies; i++) {
                const Entity& e = snap.enemies[i];
                bool active = !snap.current_turn_is_player && snap.current_turn_id == i;
                bool dead   = !e.is_alive;

                drawBox(window, ex, ey, 440, 62, PANEL,
                        active ? sf::Color(255,80,80) : BORDER, active ? 2.f : 1.f);

                if (has_font) {
                    char nb[48];
                    snprintf(nb, sizeof(nb), "Enemy %d  [key: %d]%s%s",
                             i, i, dead ? " DEAD" : "", e.is_stunned ? " STUNNED" : "");
                    drawText(window, font, nb, ex+8, ey+5, 12,
                             dead ? C_DEAD : (active ? sf::Color(255,130,130) : C_ENEMY));

                    if (!dead) {
                        drawBar(window, ex+8, ey+24, 180, 10, e.hp, e.max_hp, HP_BG, HP_FG);
                        char hb[32]; snprintf(hb, sizeof(hb), "HP %d/%d", e.hp, e.max_hp);
                        drawText(window, font, hb, ex+196, ey+22, 11, sf::Color::White);

                        drawBar(window, ex+8, ey+39, 180, 9, e.stamina, e.max_stamina, ST_BG, ST_FG);
                        int disp_st_e = std::min(e.stamina, e.max_stamina);
                        char sb[32]; snprintf(sb, sizeof(sb), "ST %d/%d  DMG %d", disp_st_e, e.max_stamina, e.damage);
                        drawText(window, font, sb, ex+196, ey+37, 11, sf::Color(200,200,90));
                    }
                }
                ey += 68;
                // Since height is 750, 9 enemies (9*68 = 612) fit perfectly in one column
            }

            // Game-over overlay
            if (!snap.game_running) {
                drawRect(window, 0, 0, 900, 750, sf::Color(0,0,0,180));
                if (has_font) {
                    drawText(window, font, snap.log_message, 170, 335, 28, sf::Color::White);
                    drawText(window, font, "Press ESC to exit.", 350, 385, 18, sf::Color(180,180,180));
                }
            }
        }

        window.display();
    }

    running = false;
    if (threads_started) {
        custom_lock_acquire(&global_state->custom_global_lock);
        int np = global_state->num_players;
        custom_lock_release(&global_state->custom_global_lock);
        for (int i = 0; i < np; i++) pthread_join(player_threads[i], NULL);
    }
    munmap(global_state, sizeof(GameState));
    return 0;
}
