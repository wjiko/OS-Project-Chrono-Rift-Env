#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <thread>
#include <vector>
#include <signal.h>
#include <cstdlib>
#include "../shared.h"

GameState* global_state = nullptr;
bool running = true;

// Signal handler for Stun Mechanic
void handle_stun(int sig) {
    std::this_thread::sleep_for(std::chrono::seconds(3));
}

void enemy_thread(int enemy_id) {
    while (running) {
        sem_wait(&global_state->mutex);
        
        if (!global_state->game_running) {
            running = false;
            sem_post(&global_state->mutex);
            break;
        }
        
        if (global_state->enemies[enemy_id].pid == 0) {
            global_state->enemies[enemy_id].pid = getpid();
        }

        // Check if it's my turn
        if (global_state->current_turn_id == enemy_id && !global_state->current_turn_is_player && !global_state->action_submitted) {
            
            int target = -1;
            for (int i=0; i<global_state->num_players; i++) {
                if (global_state->players[i].is_alive) {
                    target = i;
                    break;
                }
            }
            
            if (target != -1 && (rand() % 100) > 10) { // 90% chance to attack
                global_state->action_type = ACTION_STRIKE;
                global_state->action_target_id = target;
            } else {
                global_state->action_type = ACTION_SKIP; // 10% chance to skip
            }
            
            global_state->action_submitted = true;
        }
        
        sem_post(&global_state->mutex);
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Polling delay
    }
}

int main() {
    signal(SIGUSR1, handle_stun);

    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd == -1) {
        std::cout << "Waiting for Arbiter...\n";
        sleep(2);
        shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (shm_fd == -1) return 1;
    }
    
    global_state = (GameState*)mmap(0, sizeof(GameState), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    while (true) {
        sem_wait(&global_state->mutex);
        if (global_state->game_running) {
            sem_post(&global_state->mutex);
            break;
        }
        sem_post(&global_state->mutex);
        sleep(1);
    }

    std::vector<std::thread> threads;
    int num_enemies = global_state->num_enemies;
    
    for (int i = 0; i < num_enemies; i++) {
        threads.push_back(std::thread(enemy_thread, i));
    }
    
    for (auto& th : threads) {
        th.join();
    }

    munmap(global_state, sizeof(GameState));
    return 0;
}
