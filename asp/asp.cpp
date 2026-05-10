// =============================================================================
//  ASP — Automated Strategic Process
//  Chrono Rift | CS2006 Operating Systems | Spring 2026
// =============================================================================
//
//  This process manages ALL NPC (enemy) AI decision making.
//  Each enemy gets its own dedicated pthread — concurrent but synchronized.
//
//  SUSPENSION PROTOCOL
//  -------------------
//  When a player uses the Ultimate Ability, the Arbiter sends SIGSTOP to this
//  process (using asp_pid stored in shared memory). This suspends ALL threads
//  inside ASP for exactly 10 seconds. The Arbiter then sends SIGCONT via its
//  SIGALRM handler to resume.
//
//  This is signal-only enforcement — no flags or pipes used for this.
//
//  SIGNAL HANDLING
//  ---------------
//  SIGUSR1  ─ Stun: an enemy has been stunned for 3 seconds.
//  SIGTERM  ─ Game ended; threads exit cleanly.
//
// =============================================================================

#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <cstdlib>
#include <ctime>
#include "../shared.h"

GameState* global_state = nullptr;
volatile bool running = true;

// ── Signal Handlers ───────────────────────────────────────────────────────────
void handle_stun(int) {
    // SIGUSR1: pause for 3 seconds (enemy stun)
    sleep(3);
}

void handle_sigterm(int) {
    running = false;
}

// ── Enemy AI Thread ───────────────────────────────────────────────────────────
// One thread per enemy. Submits Strike or Skip when it is that enemy's turn.
void* enemy_thread(void* arg) {
    int enemy_id = *(int*)arg;

    while (running) {
        custom_lock_acquire(&global_state->custom_global_lock);

        if (!global_state->game_running) {
            running = false;
            custom_lock_release(&global_state->custom_global_lock);
            break;
        }

        // Register PID so Arbiter can send SIGUSR1 stuns to this enemy
        if (global_state->enemies[enemy_id].pid == 0)
            global_state->enemies[enemy_id].pid = getpid();

        bool my_turn = (global_state->current_turn_id == enemy_id &&
                        !global_state->current_turn_is_player &&
                        !global_state->action_submitted &&
                        global_state->enemies[enemy_id].is_alive);
        custom_lock_release(&global_state->custom_global_lock);

        if (my_turn) {
            usleep(200000); // Small delay so Arbiter can't timeout before we respond

            custom_lock_acquire(&global_state->custom_global_lock);

            // Collect alive targets
            int targets[MAX_PLAYERS];
            int tc = 0;
            for (int i = 0; i < global_state->num_players; i++)
                if (global_state->players[i].is_alive)
                    targets[tc++] = i;

            if (tc > 0 && (rand() % 100) > 10) {
                // Strike a random alive player
                global_state->action_type      = ACTION_STRIKE;
                global_state->action_target_id = targets[rand() % tc];
            } else {
                // 10% chance to skip
                global_state->action_type = ACTION_SKIP;
            }
            global_state->action_submitted = true;
            custom_lock_release(&global_state->custom_global_lock);
        } else {
            custom_lock_release(&global_state->custom_global_lock);
            usleep(80000); // 80ms idle
        }
    }
    return NULL;
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main() {
    signal(SIGUSR1, handle_stun);
    signal(SIGINT,  handle_sigterm);
    signal(SIGTERM, handle_sigterm);
    srand(time(NULL) ^ getpid());

    // Connect to shared memory
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    int retries = 8;
    while (shm_fd == -1 && retries-- > 0) {
        std::cout << "ASP: waiting for Arbiter...\n";
        sleep(1);
        shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    }
    if (shm_fd == -1) { perror("shm_open"); return 1; }

    global_state = (GameState*)mmap(0, sizeof(GameState),
                                    PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (global_state == MAP_FAILED) { perror("mmap"); return 1; }

    // Register this process's PID so Arbiter can SIGSTOP/SIGCONT it
    custom_lock_acquire(&global_state->custom_global_lock);
    global_state->asp_pid = getpid();
    custom_lock_release(&global_state->custom_global_lock);

    // Wait for game to start
    while (running) {
        custom_lock_acquire(&global_state->custom_global_lock);
        bool go = global_state->game_running;
        custom_lock_release(&global_state->custom_global_lock);
        if (go) break;
        sleep(1);
    }

    int num_enemies = global_state->num_enemies;
    std::cout << "ASP: game running. Spawning " << num_enemies << " enemy threads.\n";

    pthread_t threads[MAX_ENEMIES];
    int       thread_args[MAX_ENEMIES];

    for (int i = 0; i < num_enemies; i++) {
        thread_args[i] = i;
        pthread_create(&threads[i], NULL, enemy_thread, &thread_args[i]);
    }

    for (int i = 0; i < num_enemies; i++)
        pthread_join(threads[i], NULL);

    munmap(global_state, sizeof(GameState));
    std::cout << "ASP: exiting.\n";
    return 0;
}
