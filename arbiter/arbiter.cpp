#include <iostream>
#include <fstream>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <pthread.h>
#include <signal.h>
#include "../shared.h"

GameState* global_state = nullptr;

// Helper to write to log file and terminal
void write_log_file(const std::string& msg) {
    // Print LIVE to the terminal so the user can read what is happening
    std::cout << msg << std::endl; 
    
    // Also save to file
    std::ofstream out("game_events.log", std::ios::app);
    if (out.is_open()) {
        out << msg << std::endl;
    }
}

// ===== SIGNAL HANDLERS =====

void handle_alarm(int) {
    // SIGALRM: Ultimate ability window expired — resume ASP via SIGCONT
    if (global_state) {
        custom_lock_acquire(&global_state->custom_global_lock);
        global_state->asp_suspended = false;
        snprintf(global_state->log_message, sizeof(global_state->log_message),
                 "Ultimate Ability Ended. Enemies Resumed.");
        custom_lock_release(&global_state->custom_global_lock);
        // Send SIGCONT to ASP process to wake it up
        if (global_state->asp_pid > 0) {
            kill(global_state->asp_pid, SIGCONT);
        }
    }
}

void handle_sigterm(int) {
    if (global_state) {
        custom_lock_acquire(&global_state->custom_global_lock);
        global_state->game_running = false;
        snprintf(global_state->log_message, sizeof(global_state->log_message),
                 "Game forcibly terminated (SIGTERM/SIGINT).");
        custom_lock_release(&global_state->custom_global_lock);
    }
}

// ===== DEADLOCK DETECTOR THREAD =====

void* deadlock_detector(void* arg) {
    (void)arg;
    while (global_state && global_state->game_running) {
        sleep(2);
        custom_lock_acquire(&global_state->custom_global_lock);

        // Circular wait: Solar locked by entity A waiting for Lunar, Lunar locked by entity B waiting for Solar
        bool solar_locked = (global_state->solar_core_locked_by_id != -1);
        bool lunar_locked = (global_state->lunar_blade_locked_by_id != -1);

        if (solar_locked && lunar_locked) {
            bool different = (global_state->solar_core_locked_by_player != global_state->lunar_blade_locked_by_player) ||
                             (global_state->solar_core_locked_by_id != global_state->lunar_blade_locked_by_id);
            if (different) {
                snprintf(global_state->log_message, sizeof(global_state->log_message),
                         "DEADLOCK DETECTED! Force-releasing Solar Core & Lunar Blade.");
                global_state->solar_core_locked_by_id = -1;
                global_state->lunar_blade_locked_by_id = -1;
            }
        }
        custom_lock_release(&global_state->custom_global_lock);
    }
    return NULL;
}

// ===== INVENTORY SPACE ALLOCATOR (recursive) =====

bool allocate_weapon(Entity* ent, WeaponType w) {
    int sz = get_weapon_size(w);
    if (sz == 0) return false;

    // Find contiguous free slots
    int streak = 0, start_idx = -1;
    for (int i = 0; i < 20; i++) {
        if (ent->inv.slots[i] == WPN_NONE) {
            if (streak == 0) start_idx = i;
            streak++;
            if (streak == sz) {
                for (int j = 0; j < sz; j++) ent->inv.slots[start_idx + j] = w;
                return true;
            }
        } else {
            streak = 0;
        }
    }

    // Not enough contiguous space — swap one weapon out to LTS and recurse
    for (int i = 0; i < 20; i++) {
        WeaponType existing = ent->inv.slots[i];
        if (existing != WPN_NONE) {
            int ex_sz = get_weapon_size(existing);
            if (ent->inv.lts_count < 50) {
                ent->inv.long_term_storage[ent->inv.lts_count++] = existing;
                for (int j = 0; j < ex_sz && (i + j) < 20; j++) ent->inv.slots[i + j] = WPN_NONE;
                return allocate_weapon(ent, w);
            }
        }
    }
    return false;
}

// ===== GAME INIT (exact spec formulas) =====

void init_game() {
    // Roll numbers: 24I-0523 → 523, 24I-0822 → 822
    int p_roll[2] = {523, 822};
    // Use first player's roll number as the random seed (spec requirement)
    srand(p_roll[0]);

    // Enemy count: 2-9 as per spec
    global_state->num_enemies = 2 + (rand() % 8);
    global_state->total_kills = 0;
    global_state->win_kill_target = 10; // Win by killing 10 enemies total

    // Init players — exact spec formulas
    for (int i = 0; i < global_state->num_players; i++) {
        Entity& p = global_state->players[i];
        int roll = p_roll[i % 2];
        p.id = i;
        p.is_player = true;
        // HP = Roll No + random(100, 1000)
        p.hp = roll + 100 + (rand() % 901);
        p.max_hp = p.hp;
        // Damage = last digit of roll + 10
        p.damage = (roll % 10) + 10;
        // Speed = 100 / num_players
        p.speed = 100 / global_state->num_players;
        p.max_stamina = 100;
        p.stamina = 0;
        p.is_alive = true;
        p.is_stunned = false;
        p.pid = 0;
        for (int j = 0; j < 20; j++) p.inv.slots[j] = WPN_NONE;
        p.inv.lts_count = 0;
        allocate_weapon(&p, WPN_IRON_HALBERD);
    }

    // Init enemies — exact spec formulas
    for (int i = 0; i < global_state->num_enemies; i++) {
        Entity& e = global_state->enemies[i];
        int roll = p_roll[i % 2];
        e.id = i;
        e.is_player = false;
        // HP = last 2 digits of roll + random(50, 200)
        e.hp = (roll % 100) + 50 + (rand() % 151);
        e.max_hp = e.hp;
        // Damage = second last digit of roll + 10
        e.damage = ((roll / 10) % 10) + 10;
        // Speed = random(10, 30)
        e.speed = 10 + (rand() % 21);
        e.max_stamina = 150; // spec says 150 for enemies
        e.stamina = 0;
        e.is_alive = true;
        e.is_stunned = false;
        e.pid = 0;
        for (int j = 0; j < 20; j++) e.inv.slots[j] = WPN_NONE;
        e.inv.lts_count = 0;
    }

    global_state->game_running = true;
    global_state->asp_suspended = false;
    global_state->current_turn_id = -1;
    global_state->action_submitted = false;
    global_state->solar_core_locked_by_id = -1;
    global_state->lunar_blade_locked_by_id = -1;
    global_state->weapon_drop_pending = false;
    global_state->weapon_drop_type = WPN_NONE;
    snprintf(global_state->log_message, sizeof(global_state->log_message),
             "Game Started! %d enemies. Kill 10 to win.", global_state->num_enemies);
}

// ===== MAIN GAME LOOP =====

int main() {
    signal(SIGALRM, handle_alarm);
    signal(SIGINT,  handle_sigterm);
    signal(SIGTERM, handle_sigterm);

    // Cleanup stale shm from previous run
    shm_unlink(SHM_NAME);

    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) { perror("shm_open"); return 1; }
    ftruncate(shm_fd, sizeof(GameState));
    global_state = (GameState*)mmap(0, sizeof(GameState), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    memset(global_state, 0, sizeof(GameState));

    global_state->arbiter_pid = getpid(); // Store so HIP can kill us on quit
    global_state->custom_global_lock = 0;
    global_state->setup_complete = false;
    global_state->game_running = false;
    global_state->asp_pid = 0;

    std::cout << "Arbiter: waiting for player count selection...\n";
    while (true) {
        custom_lock_acquire(&global_state->custom_global_lock);
        if (global_state->setup_complete) {
            init_game();
            custom_lock_release(&global_state->custom_global_lock);
            break;
        }
        custom_lock_release(&global_state->custom_global_lock);
        sleep(1);
    }
    std::cout << "Arbiter: game initialised with " << global_state->num_players
              << " players, " << global_state->num_enemies << " enemies.\n";

    // Start deadlock detector background thread
    pthread_t deadlock_th;
    pthread_create(&deadlock_th, NULL, deadlock_detector, NULL);

    int npc_timeout_ticks = 0; // each tick = 200ms → 15 ticks = 3 seconds
    char last_log[256] = "";

    std::cout << "\n\n======================================================\n";
    std::cout << "               WELCOME TO CHRONO RIFT!\n";
    std::cout << "======================================================\n";
    std::cout << "WHAT IS THIS GAME?\n";
    std::cout << "This is a Turn-Based Tactical RPG (like Pokemon or Final Fantasy).\n";
    std::cout << "1. Look at the yellow 'ST' (Stamina) bars. They fill up automatically.\n";
    std::cout << "2. When a player's ST bar is full, it is YOUR TURN. The game pauses.\n";
    std::cout << "3. Press a button (e.g., '0' to attack Enemy 0) to take your action.\n";
    std::cout << "4. Enemies will also attack you when their ST bar fills up.\n";
    std::cout << "5. Kill 10 enemies in total to win the game!\n";
    std::cout << "======================================================\n\n";

    write_log_file("NEW GAME STARTED: " + std::to_string(global_state->num_players) + " Players, " + std::to_string(global_state->num_enemies) + " Enemies.");
    write_log_file("======================================================\n");

    while (global_state->game_running) {
        custom_lock_acquire(&global_state->custom_global_lock);

        if (strcmp(last_log, global_state->log_message) != 0) {
            strcpy(last_log, global_state->log_message);
            write_log_file("[GAME EVENT] " + std::string(last_log));
        }

        // === Win / Lose Check ===
        int alive_players = 0;
        for (int i = 0; i < global_state->num_players; i++)
            if (global_state->players[i].is_alive) alive_players++;

        if (alive_players == 0) {
            snprintf(global_state->log_message, sizeof(global_state->log_message), "GAME OVER! All players defeated.");
            global_state->game_running = false;
            custom_lock_release(&global_state->custom_global_lock);
            break;
        }
        if (global_state->total_kills >= global_state->win_kill_target) {
            snprintf(global_state->log_message, sizeof(global_state->log_message),
                     "VICTORY! 10 enemies defeated. You win!");
            global_state->game_running = false;
            custom_lock_release(&global_state->custom_global_lock);
            break;
        }

        // === NPC Timeout (3 sec = 15 x 200ms ticks) ===
        if (global_state->current_turn_id != -1 &&
            !global_state->current_turn_is_player &&
            !global_state->action_submitted) {
            npc_timeout_ticks++;
            if (npc_timeout_ticks >= 15) {
                global_state->action_type = ACTION_SKIP;
                global_state->action_submitted = true;
                npc_timeout_ticks = 0;
                snprintf(global_state->log_message, sizeof(global_state->log_message),
                         "Enemy %d timeout — forced SKIP.", global_state->current_turn_id);
            }
        } else {
            npc_timeout_ticks = 0;
        }

        // === Scheduling: tick all staminas, pick highest over threshold ===
        if (global_state->current_turn_id == -1) {
            // Tick all entities
            for (int i = 0; i < global_state->num_players; i++) {
                Entity& p = global_state->players[i];
                if (p.is_alive && !p.is_stunned)
                    p.stamina += p.speed;
            }
            if (!global_state->asp_suspended) {
                for (int i = 0; i < global_state->num_enemies; i++) {
                    Entity& e = global_state->enemies[i];
                    if (e.is_alive && !e.is_stunned)
                        e.stamina += e.speed;
                }
            }

            // Find entity with highest stamina at or over threshold
            int best_id = -1;
            bool best_is_player = false;
            int best_stamina = -1;

            for (int i = 0; i < global_state->num_players; i++) {
                Entity& p = global_state->players[i];
                if (p.is_alive && !p.is_stunned && p.stamina >= p.max_stamina && p.stamina > best_stamina) {
                    best_stamina = p.stamina;
                    best_id = i;
                    best_is_player = true;
                }
            }
            if (!global_state->asp_suspended) {
                for (int i = 0; i < global_state->num_enemies; i++) {
                    Entity& e = global_state->enemies[i];
                    if (e.is_alive && !e.is_stunned && e.stamina >= e.max_stamina && e.stamina > best_stamina) {
                        best_stamina = e.stamina;
                        best_id = i;
                        best_is_player = false;
                    }
                }
            }

            if (best_id != -1) {
                global_state->current_turn_id = best_id;
                global_state->current_turn_is_player = best_is_player;
                // Reset stamina immediately on turn assignment
                if (best_is_player)
                    global_state->players[best_id].stamina = 0;
                else
                    global_state->enemies[best_id].stamina = 0;
                
                std::string turn_msg = "\n--- TURN ASSIGNED TO: ";
                turn_msg += best_is_player ? "Player " : "Enemy ";
                turn_msg += std::to_string(best_id) + " ---";
                write_log_file(turn_msg);
            }
        }
        // === Process submitted action ===
        else if (global_state->action_submitted) {
            Entity* actor = global_state->current_turn_is_player
                ? &global_state->players[global_state->current_turn_id]
                : &global_state->enemies[global_state->current_turn_id];
            Entity* target = nullptr;

            int tid = global_state->action_target_id;
            if (tid >= 0) {
                if (global_state->current_turn_is_player && tid < MAX_ENEMIES)
                    target = &global_state->enemies[tid];
                else if (!global_state->current_turn_is_player && tid < MAX_PLAYERS)
                    target = &global_state->players[tid];
            }

            switch (global_state->action_type) {

            case ACTION_STRIKE:
            case ACTION_USE_WEAPON: {
                if (!target) { actor->stamina = 0; break; }
                int dmg = (global_state->action_type == ACTION_USE_WEAPON)
                    ? get_weapon_damage(global_state->action_weapon)
                    : actor->damage;
                target->hp -= dmg;
                if (target->hp <= 0) {
                    target->hp = 0;
                    target->is_alive = false;
                    if (global_state->current_turn_is_player) {
                        global_state->total_kills++;

                        // Introduce Eclipse Relic as a dynamic mid-game artifact on 5th kill
                        if (global_state->total_kills == 5 && !global_state->eclipse_relic_in_world) {
                            global_state->eclipse_relic_in_world = true;
                            snprintf(global_state->log_message, sizeof(global_state->log_message),
                                     "*** ECLIPSE RELIC appears! A powerful artifact has entered the world! ***");
                        }

                        // Weapon drop — player must CHOOSE to pick it up (spec requirement)
                        WeaponType drop = (WeaponType)(1 + rand() % 7);
                        // If Eclipse Relic is in the world and not yet allocated, make it the drop
                        if (global_state->eclipse_relic_in_world && global_state->total_kills == 5)
                            drop = WPN_ECLIPSE_RELIC;

                        global_state->weapon_drop_pending = true;
                        global_state->weapon_drop_type    = drop;
                        // Keep current_turn_id active so player can answer the Y/N prompt
                        snprintf(global_state->log_message, sizeof(global_state->log_message),
                                 "P%d kills E%d! Kills:%d/10. Pick up %s? Press Y=Yes N=No",
                                 actor->id, target->id, global_state->total_kills,
                                 get_weapon_name(drop));
                    } else {
                        snprintf(global_state->log_message, sizeof(global_state->log_message),
                                 "Enemy %d defeats Player %d!", actor->id, target->id);
                    }
                } else {
                    snprintf(global_state->log_message, sizeof(global_state->log_message),
                             "%s%d hits %s%d for %d dmg! (HP: %d)",
                             actor->is_player ? "P" : "E", actor->id,
                             target->is_player ? "P" : "E", target->id,
                             dmg, target->hp);
                    // 20% stun chance on hit
                    if (rand() % 100 < 20 && target->pid > 0)
                        kill(target->pid, SIGUSR1);
                }
                actor->stamina = 0;
                break;
            }

            case ACTION_EXHAUST:
                if (target) {
                    target->stamina -= actor->damage;
                    if (target->stamina < 0) target->stamina = 0;
                    snprintf(global_state->log_message, sizeof(global_state->log_message),
                             "P%d EXHAUSTS E%d! Stamina drained by %d.", actor->id, target->id, actor->damage);
                }
                actor->stamina = 0;
                break;

            case ACTION_HEAL: {
                int heal = actor->max_hp / 10;
                actor->hp = (actor->hp + heal > actor->max_hp) ? actor->max_hp : actor->hp + heal;
                snprintf(global_state->log_message, sizeof(global_state->log_message),
                         "P%d HEALS for %d HP! (HP: %d/%d)", actor->id, heal, actor->hp, actor->max_hp);
                actor->stamina = 0;
                break;
            }

            case ACTION_SKIP:
                snprintf(global_state->log_message, sizeof(global_state->log_message),
                         "%s%d SKIPS turn.", actor->is_player ? "Player " : "Enemy ", actor->id);
                actor->stamina = actor->max_stamina / 2;
                break;

            case ACTION_SWAP_IN:
                if (actor->inv.lts_count > 0) {
                    WeaponType w = actor->inv.long_term_storage[--actor->inv.lts_count];
                    allocate_weapon(actor, w);
                    snprintf(global_state->log_message, sizeof(global_state->log_message),
                             "P%d swaps in %s from storage!", actor->id, get_weapon_name(w));
                } else {
                    snprintf(global_state->log_message, sizeof(global_state->log_message),
                             "P%d: Long-term storage is empty!", actor->id);
                }
                actor->stamina = 0;
                break;

            case ACTION_ULTIMATE: {
                bool has_solar = (global_state->solar_core_locked_by_id == actor->id &&
                                  global_state->solar_core_locked_by_player == actor->is_player);
                bool has_lunar = (global_state->lunar_blade_locked_by_id == actor->id &&
                                  global_state->lunar_blade_locked_by_player == actor->is_player);
                if (has_solar && has_lunar) {
                    global_state->asp_suspended = true;
                    // Signal-only suspension — send SIGSTOP to ASP
                    if (global_state->asp_pid > 0)
                        kill(global_state->asp_pid, SIGSTOP);
                    alarm(10); // SIGALRM after 10s will call handle_alarm → SIGCONT
                    snprintf(global_state->log_message, sizeof(global_state->log_message),
                             "P%d ULTIMATE! Enemies FROZEN 10 seconds!", actor->id);
                } else {
                    snprintf(global_state->log_message, sizeof(global_state->log_message),
                             "P%d: Need BOTH Solar Core AND Lunar Blade locked!", actor->id);
                }
                actor->stamina = 0;
                break;
            }

            case ACTION_LOCK_ARTIFACT:
                if (global_state->action_weapon == WPN_SOLAR_CORE) {
                    global_state->solar_core_locked_by_id = actor->id;
                    global_state->solar_core_locked_by_player = actor->is_player;
                    snprintf(global_state->log_message, sizeof(global_state->log_message),
                             "P%d LOCKED the Solar Core!", actor->id);
                } else if (global_state->action_weapon == WPN_LUNAR_BLADE) {
                    global_state->lunar_blade_locked_by_id = actor->id;
                    global_state->lunar_blade_locked_by_player = actor->is_player;
                    snprintf(global_state->log_message, sizeof(global_state->log_message),
                             "P%d LOCKED the Lunar Blade!", actor->id);
                }
                actor->stamina = 0;
                break;

            case ACTION_PICKUP_WEAPON:
                // Player chose to pick up the dropped weapon
                if (global_state->weapon_drop_pending) {
                    allocate_weapon(actor, global_state->weapon_drop_type);
                    snprintf(global_state->log_message, sizeof(global_state->log_message),
                             "P%d picks up %s!", actor->id, get_weapon_name(global_state->weapon_drop_type));
                    global_state->weapon_drop_pending = false;
                    global_state->weapon_drop_type    = WPN_NONE;
                }
                actor->stamina = 0;
                break;

            case ACTION_DECLINE_WEAPON:
                // Player chose NOT to pick up the dropped weapon
                snprintf(global_state->log_message, sizeof(global_state->log_message),
                         "P%d declines the weapon drop.", actor->id);
                global_state->weapon_drop_pending = false;
                global_state->weapon_drop_type    = WPN_NONE;
                actor->stamina = 0;
                break;
            }

            global_state->action_submitted = false;
            // Only end turn if no weapon drop is pending (player still needs to answer Y/N)
            if (!global_state->weapon_drop_pending) {
                global_state->current_turn_id = -1;
            }
        }

        custom_lock_release(&global_state->custom_global_lock);
        usleep(200000); // 200ms per tick
    }

    pthread_join(deadlock_th, NULL);

    std::cout << "Arbiter: game ended. Cleaning up shared memory.\n";
    munmap(global_state, sizeof(GameState));
    shm_unlink(SHM_NAME);
    return 0;
}
