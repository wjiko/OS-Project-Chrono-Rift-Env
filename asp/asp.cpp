#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <cstdlib>
#include "../shared.h"

GameState* global_state = nullptr;
bool running = true;

void handle_stun(int) {
    sleep(3);
}

void handle_sigterm(int) {
    running = false;
}

void* enemy_thread(void* arg) {
    int enemy_id = *(int*)arg;
    while (running) {
        custom_lock_acquire(&global_state->custom_global_lock);
        
        if (!global_state->game_running) {
            running = false;
            custom_lock_release(&global_state->custom_global_lock);
            break;
        }
        
        if (global_state->enemies[enemy_id].pid == 0) {
            global_state->enemies[enemy_id].pid = getpid();
        }

        if (global_state->current_turn_id == enemy_id && !global_state->current_turn_is_player && !global_state->action_submitted) {
            
            int target = -1;
            for (int i=0; i<global_state->num_players; i++) {
                if (global_state->players[i].is_alive) {
                    target = i;
                    break;
                }
            }
            
            if (target != -1 && (rand() % 100) > 10) { 
                global_state->action_type = ACTION_STRIKE;
                global_state->action_target_id = target;
            } else {
                global_state->action_type = ACTION_SKIP; 
            }
            
            global_state->action_submitted = true;
        }
        
        custom_lock_release(&global_state->custom_global_lock);
        usleep(100000); // 100ms
    }
    return NULL;
}

int main() {
    signal(SIGUSR1, handle_stun);
    signal(SIGINT, handle_sigterm);
    signal(SIGTERM, handle_sigterm);

    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd == -1) {
        std::cout << "Waiting for Arbiter...\n";
        sleep(2);
        shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (shm_fd == -1) return 1;
    }
    
    global_state = (GameState*)mmap(0, sizeof(GameState), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    while (true) {
        custom_lock_acquire(&global_state->custom_global_lock);
        if (global_state->game_running) {
            custom_lock_release(&global_state->custom_global_lock);
            break;
        }
        custom_lock_release(&global_state->custom_global_lock);
        sleep(1);
    }

    int num_enemies = global_state->num_enemies;
    pthread_t threads[MAX_ENEMIES];
    int thread_args[MAX_ENEMIES];
    
    for (int i = 0; i < num_enemies; i++) {
        thread_args[i] = i;
        pthread_create(&threads[i], NULL, enemy_thread, &thread_args[i]);
    }
    
    for (int i = 0; i < num_enemies; i++) {
        pthread_join(threads[i], NULL);
    }

    munmap(global_state, sizeof(GameState));
    return 0;
}
