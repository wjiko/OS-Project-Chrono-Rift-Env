#include <SFML/Graphics.hpp>
#include <SFML/Window.hpp>
#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include "../shared.h"

GameState* global_state = nullptr;
bool running = true;

volatile int local_input_lock = 0;
int pending_action = -1; 
int pending_target = -1;
WeaponType pending_action_weapon = WPN_NONE;

void handle_stun(int) {
    global_state->players[0].is_stunned = true;
    sleep(3);
    global_state->players[0].is_stunned = false;
}

void* player_thread(void* arg) {
    int player_id = *(int*)arg;
    while (running) {
        custom_lock_acquire(&global_state->custom_global_lock);
        if (!global_state->game_running) {
            custom_lock_release(&global_state->custom_global_lock);
            usleep(200000); 
            continue;
        }
        
        if (global_state->players[player_id].pid == 0) {
            global_state->players[player_id].pid = getpid();
        }

        if (global_state->current_turn_id == player_id && global_state->current_turn_is_player && !global_state->action_submitted) {
            custom_lock_release(&global_state->custom_global_lock);
            
            // Wait for human input using custom busy-wait spinlock instead of condition variable
            int my_action = -1;
            int my_target = -1;
            WeaponType my_weapon = WPN_NONE;
            
            while (running) {
                custom_lock_acquire(&local_input_lock);
                if (pending_action != -1) {
                    my_action = pending_action;
                    my_target = pending_target;
                    my_weapon = pending_action_weapon;
                    
                    pending_action = -1; 
                    pending_action_weapon = WPN_NONE;
                    custom_lock_release(&local_input_lock);
                    break;
                }
                custom_lock_release(&local_input_lock);
                usleep(50000); // 50ms check interval
            }
            
            if (!running) break;

            custom_lock_acquire(&global_state->custom_global_lock);
            global_state->action_type = my_action;
            global_state->action_target_id = my_target;
            global_state->action_weapon = my_weapon;
            global_state->action_submitted = true;
            custom_lock_release(&global_state->custom_global_lock);
            
        } else {
            custom_lock_release(&global_state->custom_global_lock);
        }
        usleep(50000); 
    }
    return NULL;
}

int main() {
    signal(SIGUSR1, handle_stun);

    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd == -1) {
        sleep(2);
        shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (shm_fd == -1) return 1;
    }
    
    global_state = (GameState*)mmap(0, sizeof(GameState), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    sf::RenderWindow window(sf::VideoMode(800, 600), "Chrono Rift - Human Interface");
    window.setFramerateLimit(30);
    
    sf::Font font;
    font.loadFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");

    pthread_t threads[MAX_PLAYERS];
    int thread_args[MAX_PLAYERS];
    bool threads_started = false;

    while (window.isOpen() && running) {
        sf::Event event;
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed) {
                window.close();
                running = false;
            }
            if (event.type == sf::Event::KeyPressed) {
                custom_lock_acquire(&global_state->custom_global_lock);
                if (!global_state->setup_complete) {
                    if (event.key.code >= sf::Keyboard::Num1 && event.key.code <= sf::Keyboard::Num4) {
                        global_state->num_players = event.key.code - sf::Keyboard::Num0;
                        global_state->setup_complete = true;
                    }
                } else if (global_state->game_running) {
                    custom_lock_acquire(&local_input_lock);
                    if (event.key.code >= sf::Keyboard::Num0 && event.key.code <= sf::Keyboard::Num9) {
                        pending_action = ACTION_STRIKE;
                        pending_target = event.key.code - sf::Keyboard::Num0;
                    } else if (event.key.code == sf::Keyboard::S) {
                        pending_action = ACTION_SKIP;
                    } else if (event.key.code == sf::Keyboard::H) {
                        pending_action = ACTION_HEAL;
                    } else if (event.key.code == sf::Keyboard::E) {
                        pending_action = ACTION_EXHAUST;
                        pending_target = 0; 
                    } else if (event.key.code == sf::Keyboard::U) {
                        pending_action = ACTION_ULTIMATE;
                    } else if (event.key.code == sf::Keyboard::W) {
                        pending_action = ACTION_USE_WEAPON;
                        pending_target = 0; 
                        WeaponType wpn = WPN_NONE;
                        for(int i=0; i<20; i++) {
                            if (global_state->players[global_state->current_turn_id].inv.slots[i] != WPN_NONE) {
                                wpn = global_state->players[global_state->current_turn_id].inv.slots[i];
                                break;
                            }
                        }
                        pending_action_weapon = wpn;
                    } else if (event.key.code == sf::Keyboard::I) {
                        pending_action = ACTION_SWAP_IN;
                    } else if (event.key.code == sf::Keyboard::L) {
                        pending_action = ACTION_LOCK_ARTIFACT;
                        pending_action_weapon = WPN_SOLAR_CORE; 
                    }
                    custom_lock_release(&local_input_lock);
                }
                custom_lock_release(&global_state->custom_global_lock);
            }
        }
        
        window.clear(sf::Color(30, 30, 30));
        
        custom_lock_acquire(&global_state->custom_global_lock);
        
        if (!global_state->setup_complete) {
            sf::Text title("CHRONO RIFT", font, 40);
            title.setPosition(250, 200);
            window.draw(title);
            sf::Text prompt("Press 1, 2, 3, or 4 to select Party Size.", font, 20);
            prompt.setPosition(200, 300);
            window.draw(prompt);
        } else {
            if (!threads_started && global_state->game_running) {
                for (int i = 0; i < global_state->num_players; i++) {
                    thread_args[i] = i;
                    pthread_create(&threads[i], NULL, player_thread, &thread_args[i]);
                }
                threads_started = true;
            }

            sf::Text logTxt(global_state->log_message, font, 20);
            logTxt.setPosition(10, 10);
            window.draw(logTxt);
            
            int y = 50;
            sf::Text pTxt("PLAYERS", font, 18);
            pTxt.setPosition(10, y);
            window.draw(pTxt);
            y += 30;
            
            for (int i=0; i<global_state->num_players; i++) {
                sf::RectangleShape hpBg(sf::Vector2f(200, 15));
                hpBg.setPosition(10, y); hpBg.setFillColor(sf::Color::Red);
                window.draw(hpBg);
                
                float hpPct = (float)global_state->players[i].hp / global_state->players[i].max_hp;
                sf::RectangleShape hpFg(sf::Vector2f(200 * std::max(0.0f, hpPct), 15));
                hpFg.setPosition(10, y); hpFg.setFillColor(sf::Color::Green);
                window.draw(hpFg);
                
                sf::RectangleShape stBg(sf::Vector2f(200, 10));
                stBg.setPosition(10, y + 20); stBg.setFillColor(sf::Color(100, 100, 100));
                window.draw(stBg);
                
                float stPct = (float)global_state->players[i].stamina / global_state->players[i].max_stamina;
                sf::RectangleShape stFg(sf::Vector2f(200 * std::max(0.0f, stPct), 10));
                stFg.setPosition(10, y + 20); stFg.setFillColor(sf::Color::Yellow);
                window.draw(stFg);
                
                char buf[128];
                snprintf(buf, sizeof(buf), "P%d (HP: %d) (Stam: %d)", i, global_state->players[i].hp, global_state->players[i].stamina);
                sf::Text info(buf, font, 14);
                info.setPosition(220, y);
                window.draw(info);
                
                if (global_state->current_turn_is_player && global_state->current_turn_id == i) {
                    sf::Text turnTxt("<- YOUR TURN! 0-9(Strike), W(Weapon), I(Swap), H(Heal), E(Exhaust), U(Ult), L(Lock)", font, 12);
                    turnTxt.setFillColor(sf::Color::Yellow);
                    turnTxt.setPosition(420, y);
                    window.draw(turnTxt);
                }
                y += 40;
            }
            
            y += 20;
            sf::Text eTxt("ENEMIES", font, 18);
            eTxt.setPosition(10, y);
            window.draw(eTxt);
            y += 30;
            
            for (int i=0; i<global_state->num_enemies; i++) {
                if (!global_state->enemies[i].is_alive) continue;
                sf::RectangleShape hpBg(sf::Vector2f(200, 15));
                hpBg.setPosition(10, y); hpBg.setFillColor(sf::Color::Red);
                window.draw(hpBg);
                
                float hpPct = (float)global_state->enemies[i].hp / global_state->enemies[i].max_hp;
                sf::RectangleShape hpFg(sf::Vector2f(200 * std::max(0.0f, hpPct), 15));
                hpFg.setPosition(10, y); hpFg.setFillColor(sf::Color::Green);
                window.draw(hpFg);
                
                char buf[128];
                snprintf(buf, sizeof(buf), "E%d (HP: %d)", i, global_state->enemies[i].hp);
                sf::Text info(buf, font, 14);
                info.setPosition(220, y);
                window.draw(info);
                y += 25;
            }
        }
        
        custom_lock_release(&global_state->custom_global_lock);
        window.display();
    }

    running = false;
    
    if (threads_started) {
        for (int i = 0; i < global_state->num_players; i++) pthread_join(threads[i], NULL);
    }
    
    munmap(global_state, sizeof(GameState));
    return 0;
}
