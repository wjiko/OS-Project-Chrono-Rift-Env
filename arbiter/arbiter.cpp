#include <iostream>
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

void* deadlock_detector(void* arg) {
    while (global_state && global_state->game_running) {
        sleep(2);
        custom_lock_acquire(&global_state->custom_global_lock);
        
        if (global_state->solar_core_locked_by_id != -1 && global_state->lunar_blade_locked_by_id != -1) {
            bool different_entities = (global_state->solar_core_locked_by_player != global_state->lunar_blade_locked_by_player) ||
                                      (global_state->solar_core_locked_by_id != global_state->lunar_blade_locked_by_id);
            if (different_entities) {
                snprintf(global_state->log_message, sizeof(global_state->log_message), 
                         "DEADLOCK DETECTED! Forcing release of Solar Core and Lunar Blade.");
                global_state->solar_core_locked_by_id = -1;
                global_state->lunar_blade_locked_by_id = -1;
            }
        }
        custom_lock_release(&global_state->custom_global_lock);
    }
    return NULL;
}

void handle_alarm(int) {
    if (global_state) {
        custom_lock_acquire(&global_state->custom_global_lock);
        global_state->asp_suspended = false;
        snprintf(global_state->log_message, sizeof(global_state->log_message), "Ultimate Ability Ended. Enemies Resumed.");
        custom_lock_release(&global_state->custom_global_lock);
    }
}

bool allocate_weapon(Entity* ent, WeaponType w) {
    int sz = get_weapon_size(w);
    int streak = 0;
    int start_idx = -1;
    for (int i=0; i<20; i++) {
        if (ent->inv.slots[i] == WPN_NONE) {
            if (streak == 0) start_idx = i;
            streak++;
            if (streak == sz) {
                for (int j=0; j<sz; j++) ent->inv.slots[start_idx+j] = w;
                return true;
            }
        } else {
            streak = 0;
        }
    }
    
    for (int i=0; i<20; i++) {
        WeaponType existing = ent->inv.slots[i];
        if (existing != WPN_NONE) {
            int ex_sz = get_weapon_size(existing);
            ent->inv.long_term_storage[ent->inv.lts_count++] = existing;
            for (int j=0; j<ex_sz && (i+j)<20; j++) ent->inv.slots[i+j] = WPN_NONE;
            return allocate_weapon(ent, w);
        }
    }
    return false;
}

void init_game() {
    int p_roll[2] = {523, 822}; 
    srand(time(NULL));
    global_state->num_enemies = 2 + (rand() % 8); 
    
    for (int i = 0; i < global_state->num_players; i++) {
        Entity& p = global_state->players[i];
        int roll = p_roll[i % 2];
        p.id = i;
        p.is_player = true;
        p.hp = roll + (100 + rand() % 901);
        p.max_hp = p.hp;
        p.damage = (roll % 10) + 10;
        p.speed = 100 / global_state->num_players;
        p.max_stamina = 100;
        p.stamina = 0;
        p.is_alive = true;
        p.is_stunned = false;
        for(int j=0; j<20; j++) p.inv.slots[j] = WPN_NONE;
        p.inv.lts_count = 0;
        
        allocate_weapon(&p, WPN_IRON_HALBERD);
    }
    
    for (int i = 0; i < global_state->num_enemies; i++) {
        Entity& e = global_state->enemies[i];
        int roll = p_roll[i % 2];
        e.id = i;
        e.is_player = false;
        e.hp = (roll % 100) + (50 + rand() % 151);
        e.max_hp = e.hp;
        e.damage = ((roll / 10) % 10) + 10;
        e.speed = 10 + (rand() % 21);
        e.max_stamina = 150;
        e.stamina = 0;
        e.is_alive = true;
        e.is_stunned = false;
        for(int j=0; j<20; j++) e.inv.slots[j] = WPN_NONE;
        e.inv.lts_count = 0;
    }
    
    global_state->game_running = true;
    global_state->asp_suspended = false;
    global_state->current_turn_id = -1;
    global_state->action_submitted = false;
    global_state->solar_core_locked_by_id = -1;
    global_state->lunar_blade_locked_by_id = -1;
    snprintf(global_state->log_message, sizeof(global_state->log_message), "Game Started!");
}

int main() {
    signal(SIGALRM, handle_alarm);

    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) return 1;
    ftruncate(shm_fd, sizeof(GameState));
    global_state = (GameState*)mmap(0, sizeof(GameState), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    
    global_state->custom_global_lock = 0; 
    global_state->setup_complete = false;
    global_state->game_running = false;
    
    std::cout << "Arbiter waiting for setup...\n";
    while(true) {
        custom_lock_acquire(&global_state->custom_global_lock);
        if (global_state->setup_complete) {
            init_game();
            custom_lock_release(&global_state->custom_global_lock);
            break;
        }
        custom_lock_release(&global_state->custom_global_lock);
        sleep(1);
    }

    pthread_t deadlock_th;
    pthread_create(&deadlock_th, NULL, deadlock_detector, NULL);
    
    int npc_timeout_ticks = 0;

    while (global_state->game_running) {
        custom_lock_acquire(&global_state->custom_global_lock);
        
        int alive_players = 0;
        for (int i=0; i<global_state->num_players; i++) if (global_state->players[i].is_alive) alive_players++;
        int alive_enemies = 0;
        for (int i=0; i<global_state->num_enemies; i++) if (global_state->enemies[i].is_alive) alive_enemies++;
        
        if (alive_players == 0 || alive_enemies == 0) {
            snprintf(global_state->log_message, sizeof(global_state->log_message), 
                     alive_players == 0 ? "GAME OVER! Enemies win." : "VICTORY! Players win.");
            global_state->game_running = false;
            custom_lock_release(&global_state->custom_global_lock);
            break;
        }
        
        if (global_state->current_turn_id != -1 && !global_state->current_turn_is_player && !global_state->action_submitted) {
            npc_timeout_ticks++;
            if (npc_timeout_ticks >= 15) { 
                global_state->action_type = ACTION_SKIP;
                global_state->action_submitted = true;
                npc_timeout_ticks = 0;
                snprintf(global_state->log_message, sizeof(global_state->log_message), "Enemy %d took too long! TIMEOUT FORCED SKIP.", global_state->current_turn_id);
            }
        } else {
            npc_timeout_ticks = 0;
        }
        
        if (global_state->current_turn_id == -1) {
            bool turn_assigned = false;
            for (int i = 0; i < global_state->num_players && !turn_assigned; i++) {
                if (global_state->players[i].is_alive && !global_state->players[i].is_stunned) {
                    global_state->players[i].stamina += global_state->players[i].speed;
                    if (global_state->players[i].stamina >= global_state->players[i].max_stamina) {
                        global_state->players[i].stamina = global_state->players[i].max_stamina;
                        global_state->current_turn_id = i;
                        global_state->current_turn_is_player = true;
                        turn_assigned = true;
                    }
                }
            }
            for (int i = 0; i < global_state->num_enemies && !turn_assigned && !global_state->asp_suspended; i++) {
                if (global_state->enemies[i].is_alive && !global_state->enemies[i].is_stunned) {
                    global_state->enemies[i].stamina += global_state->enemies[i].speed;
                    if (global_state->enemies[i].stamina >= global_state->enemies[i].max_stamina) {
                        global_state->enemies[i].stamina = global_state->enemies[i].max_stamina;
                        global_state->current_turn_id = i;
                        global_state->current_turn_is_player = false;
                        turn_assigned = true;
                    }
                }
            }
        } 
        else if (global_state->action_submitted) {
            Entity* actor = global_state->current_turn_is_player ? &global_state->players[global_state->current_turn_id] : &global_state->enemies[global_state->current_turn_id];
            Entity* target = nullptr;
            
            if (global_state->action_target_id != -1 && global_state->action_target_id < MAX_ENEMIES) {
                target = global_state->current_turn_is_player ? &global_state->enemies[global_state->action_target_id] : &global_state->players[global_state->action_target_id];
            }

            if (global_state->action_type == ACTION_STRIKE || global_state->action_type == ACTION_USE_WEAPON) {
                int dmg = actor->damage;
                if (global_state->action_type == ACTION_USE_WEAPON) {
                    dmg = get_weapon_damage(global_state->action_weapon);
                }
                
                target->hp -= dmg;
                if (target->hp <= 0) {
                    target->hp = 0;
                    target->is_alive = false;
                    if (global_state->current_turn_is_player) {
                        WeaponType drop = (WeaponType)((rand() % 9) + 1); 
                        allocate_weapon(actor, drop);
                        snprintf(global_state->log_message, sizeof(global_state->log_message), "P%d kills E%d & gets %s!", actor->id, target->id, get_weapon_name(drop));
                    }
                } else {
                    snprintf(global_state->log_message, sizeof(global_state->log_message), "%s %d strikes %s %d for %d dmg!", 
                        actor->is_player?"Player":"Enemy", actor->id, target->is_player?"Player":"Enemy", target->id, dmg);
                    
                    if (rand() % 100 < 20 && target->pid > 0) kill(target->pid, SIGUSR1);
                }
                actor->stamina = 0;
            } 
            else if (global_state->action_type == ACTION_EXHAUST) {
                target->stamina -= actor->damage;
                if (target->stamina < 0) target->stamina = 0;
                snprintf(global_state->log_message, sizeof(global_state->log_message), "Player %d exhausts Enemy %d!", actor->id, target->id);
                actor->stamina = 0;
            }
            else if (global_state->action_type == ACTION_HEAL) {
                int heal = actor->max_hp / 10;
                actor->hp += heal;
                if (actor->hp > actor->max_hp) actor->hp = actor->max_hp;
                snprintf(global_state->log_message, sizeof(global_state->log_message), "Player %d heals for %d!", actor->id, heal);
                actor->stamina = 0;
            }
            else if (global_state->action_type == ACTION_SKIP) {
                snprintf(global_state->log_message, sizeof(global_state->log_message), "%s %d skips turn.", actor->is_player?"Player":"Enemy", actor->id);
                actor->stamina = actor->max_stamina / 2;
            }
            else if (global_state->action_type == ACTION_SWAP_IN) {
                if (actor->inv.lts_count > 0) {
                    WeaponType to_swap = actor->inv.long_term_storage[--actor->inv.lts_count];
                    allocate_weapon(actor, to_swap);
                    snprintf(global_state->log_message, sizeof(global_state->log_message), "Player %d swapped in %s!", actor->id, get_weapon_name(to_swap));
                } else {
                    snprintf(global_state->log_message, sizeof(global_state->log_message), "Player %d has nothing in storage!", actor->id);
                }
                actor->stamina = 0;
            }
            else if (global_state->action_type == ACTION_ULTIMATE) {
                bool has_both = (global_state->solar_core_locked_by_id == actor->id && global_state->solar_core_locked_by_player == actor->is_player) &&
                                (global_state->lunar_blade_locked_by_id == actor->id && global_state->lunar_blade_locked_by_player == actor->is_player);
                if (has_both) {
                    global_state->asp_suspended = true;
                    alarm(10);
                    snprintf(global_state->log_message, sizeof(global_state->log_message), "Player %d CASTS ULTIMATE! Enemies frozen for 10s!", actor->id);
                } else {
                    snprintf(global_state->log_message, sizeof(global_state->log_message), "Player %d tried Ultimate but lacks Locked Artifacts!", actor->id);
                }
                actor->stamina = 0;
            }
            else if (global_state->action_type == ACTION_LOCK_ARTIFACT) {
                if (global_state->action_weapon == WPN_SOLAR_CORE) {
                    global_state->solar_core_locked_by_id = actor->id;
                    global_state->solar_core_locked_by_player = actor->is_player;
                    snprintf(global_state->log_message, sizeof(global_state->log_message), "Player %d LOCKED Solar Core!", actor->id);
                } else if (global_state->action_weapon == WPN_LUNAR_BLADE) {
                    global_state->lunar_blade_locked_by_id = actor->id;
                    global_state->lunar_blade_locked_by_player = actor->is_player;
                    snprintf(global_state->log_message, sizeof(global_state->log_message), "Player %d LOCKED Lunar Blade!", actor->id);
                }
                actor->stamina = 0;
            }
            
            global_state->action_submitted = false;
            global_state->current_turn_id = -1;
        }

        custom_lock_release(&global_state->custom_global_lock);
        usleep(200000); // 200ms
    }

    pthread_join(deadlock_th, NULL);
    munmap(global_state, sizeof(GameState));
    shm_unlink(SHM_NAME);
    return 0;
}
