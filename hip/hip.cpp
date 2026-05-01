#include <SFML/Graphics.hpp>
#include <SFML/Window.hpp>
#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <thread>
#include <vector>
#include <signal.h>
#include <mutex>
#include <condition_variable>
#include "../shared.h"

GameState* global_state = nullptr;
bool running = true;

int pending_action = -1; 
int pending_target = -1;
WeaponType pending_action_weapon = WPN_NONE;
std::mutex input_mtx;
std::condition_variable input_cv;

void handle_stun(int sig) {
    global_state->players[0].is_stunned = true; // simplified
    std::this_thread::sleep_for(std::chrono::seconds(3));
    global_state->players[0].is_stunned = false;
}

void player_thread(int player_id) {
    while (running) {
        sem_wait(&global_state->mutex);
        if (!global_state->game_running) {
            sem_post(&global_state->mutex);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        
        if (global_state->players[player_id].pid == 0) {
            global_state->players[player_id].pid = getpid();
        }

        if (global_state->current_turn_id == player_id && global_state->current_turn_is_player && !global_state->action_submitted) {
            sem_post(&global_state->mutex);
            
            std::unique_lock<std::mutex> lock(input_mtx);
            input_cv.wait(lock, []{ return pending_action != -1 || !running; });
            if (!running) break;

            sem_wait(&global_state->mutex);
            global_state->action_type = pending_action;
            global_state->action_target_id = pending_target;
            global_state->action_weapon = pending_action_weapon;
            global_state->action_submitted = true;
            sem_post(&global_state->mutex);
            
            pending_action = -1; 
            pending_action_weapon = WPN_NONE; 
        } else {
            sem_post(&global_state->mutex);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
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

    std::vector<std::thread> threads;
    bool threads_started = false;

    while (window.isOpen() && running) {
        sf::Event event;
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed) {
                window.close();
                running = false;
                input_cv.notify_all();
            }
            if (event.type == sf::Event::KeyPressed) {
                sem_wait(&global_state->mutex);
                if (!global_state->setup_complete) {
                    if (event.key.code >= sf::Keyboard::Num1 && event.key.code <= sf::Keyboard::Num4) {
                        global_state->num_players = event.key.code - sf::Keyboard::Num0;
                        global_state->setup_complete = true;
                    }
                } else if (global_state->game_running) {
                    std::lock_guard<std::mutex> lock(input_mtx);
                    if (event.key.code >= sf::Keyboard::Num0 && event.key.code <= sf::Keyboard::Num9) {
                        pending_action = ACTION_STRIKE;
                        pending_target = event.key.code - sf::Keyboard::Num0;
                        input_cv.notify_all();
                    } else if (event.key.code == sf::Keyboard::S) {
                        pending_action = ACTION_SKIP;
                        input_cv.notify_all();
                    } else if (event.key.code == sf::Keyboard::H) {
                        pending_action = ACTION_HEAL;
                        input_cv.notify_all();
                    } else if (event.key.code == sf::Keyboard::E) {
                        pending_action = ACTION_EXHAUST;
                        pending_target = 0; // default to enemy 0
                        input_cv.notify_all();
                    } else if (event.key.code == sf::Keyboard::U) {
                        pending_action = ACTION_ULTIMATE;
                        input_cv.notify_all();
                    } else if (event.key.code == sf::Keyboard::W) {
                        pending_action = ACTION_USE_WEAPON;
                        pending_target = 0; // target enemy 0 for simplicity
                        
                        WeaponType wpn = WPN_NONE;
                        for(int i=0; i<20; i++) {
                            if (global_state->players[global_state->current_turn_id].inv.slots[i] != WPN_NONE) {
                                wpn = global_state->players[global_state->current_turn_id].inv.slots[i];
                                break;
                            }
                        }
                        pending_action_weapon = wpn;
                        input_cv.notify_all();
                    } else if (event.key.code == sf::Keyboard::I) {
                        pending_action = ACTION_SWAP_IN;
                        input_cv.notify_all();
                    } else if (event.key.code == sf::Keyboard::L) {
                        pending_action = ACTION_LOCK_ARTIFACT;
                        pending_action_weapon = WPN_SOLAR_CORE; // Try solar core
                        input_cv.notify_all();
                    }
                }
                sem_post(&global_state->mutex);
            }
        }
        
        window.clear(sf::Color(30, 30, 30));
        
        sem_wait(&global_state->mutex);
        
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
                    threads.push_back(std::thread(player_thread, i));
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
        
        sem_post(&global_state->mutex);
        window.display();
    }

    running = false;
    input_cv.notify_all();
    for (auto& th : threads) th.join();
    munmap(global_state, sizeof(GameState));
    return 0;
}
