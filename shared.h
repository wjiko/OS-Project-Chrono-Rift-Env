#pragma once

#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>

#define SHM_NAME "/chronorift_shm_2026_testset"
#define MAX_PLAYERS 4
#define MAX_ENEMIES 9

#define ACTION_STRIKE 0
#define ACTION_EXHAUST 1
#define ACTION_SKIP 2
#define ACTION_HEAL 3
#define ACTION_USE_WEAPON 4
#define ACTION_SWAP_IN 5
#define ACTION_ULTIMATE 6
#define ACTION_LOCK_ARTIFACT 7

enum WeaponType {
    WPN_NONE = 0,
    WPN_SOLAR_CORE,
    WPN_LUNAR_BLADE,
    WPN_IRON_HALBERD,
    WPN_VENOM_DAGGER,
    WPN_THUNDERSTAFF,
    WPN_OBSIDIAN_AXE,
    WPN_FROSTBOW,
    WPN_SPLINTER_STICK,
    WPN_ECLIPSE_RELIC
};

struct Inventory {
    WeaponType slots[20];
    WeaponType long_term_storage[50];
    int lts_count;
};

struct Entity {
    int id;               
    bool is_player;
    int hp;
    int max_hp;
    int damage;
    int speed;
    int stamina;
    int max_stamina;
    bool is_alive;
    bool is_stunned;      
    pid_t pid;            
    Inventory inv;
};

struct GameState {
    volatile int custom_global_lock; 
    
    bool setup_complete;
    int num_players;
    int num_enemies;
    
    struct Entity players[MAX_PLAYERS];
    struct Entity enemies[MAX_ENEMIES];
    
    bool game_running;    
    bool asp_suspended;   
    
    int current_turn_id;        
    bool current_turn_is_player; 
    
    bool action_submitted;
    int action_type;
    int action_target_id;
    WeaponType action_weapon;
    
    int solar_core_locked_by_id; 
    bool solar_core_locked_by_player;
    
    int lunar_blade_locked_by_id;
    bool lunar_blade_locked_by_player;
    
    char log_message[256];
};

static inline void custom_lock_acquire(volatile int* lock) {
    while (__sync_lock_test_and_set(lock, 1)) {
        while (*lock) {
            usleep(10); 
        }
    }
}

static inline void custom_lock_release(volatile int* lock) {
    __sync_lock_release(lock);
}

static inline int get_weapon_size(WeaponType w) {
    switch(w) {
        case WPN_SOLAR_CORE: return 10;
        case WPN_LUNAR_BLADE: return 10;
        case WPN_ECLIPSE_RELIC: return 5;
        case WPN_IRON_HALBERD: return 7;
        case WPN_VENOM_DAGGER: return 4;
        case WPN_THUNDERSTAFF: return 6;
        case WPN_OBSIDIAN_AXE: return 5;
        case WPN_FROSTBOW: return 6;
        case WPN_SPLINTER_STICK: return 2;
        default: return 0;
    }
}

static inline int get_weapon_damage(WeaponType w) {
    switch(w) {
        case WPN_SOLAR_CORE: return 95;
        case WPN_LUNAR_BLADE: return 90;
        case WPN_ECLIPSE_RELIC: return 100;
        case WPN_IRON_HALBERD: return 55;
        case WPN_VENOM_DAGGER: return 30;
        case WPN_THUNDERSTAFF: return 50;
        case WPN_OBSIDIAN_AXE: return 45;
        case WPN_FROSTBOW: return 48;
        case WPN_SPLINTER_STICK: return 12;
        default: return 0;
    }
}

static inline const char* get_weapon_name(WeaponType w) {
    switch(w) {
        case WPN_SOLAR_CORE: return "Solar Core";
        case WPN_LUNAR_BLADE: return "Lunar Blade";
        case WPN_ECLIPSE_RELIC: return "Eclipse Relic";
        case WPN_IRON_HALBERD: return "Iron Halberd";
        case WPN_VENOM_DAGGER: return "Venom Dagger";
        case WPN_THUNDERSTAFF: return "Thunderstaff";
        case WPN_OBSIDIAN_AXE: return "Obsidian Axe";
        case WPN_FROSTBOW: return "Frostbow";
        case WPN_SPLINTER_STICK: return "Splinter Stick";
        default: return "None";
    }
}
