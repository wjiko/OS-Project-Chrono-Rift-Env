#pragma once

#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <cstring>

#define SHM_NAME "/chronorift_shm_2026"
#define MAX_PLAYERS 4
#define MAX_ENEMIES 9

// ===== ACTION TYPES =====
#define ACTION_STRIKE        0
#define ACTION_EXHAUST       1
#define ACTION_SKIP          2
#define ACTION_HEAL          3
#define ACTION_USE_WEAPON    4
#define ACTION_SWAP_IN       5
#define ACTION_ULTIMATE      6
#define ACTION_LOCK_ARTIFACT 7
#define ACTION_PICKUP_WEAPON 8   // Player chooses to pick up dropped weapon
#define ACTION_DECLINE_WEAPON 9  // Player declines dropped weapon

// ===== WEAPON TYPES =====
// Slot sizes and damage are from the official project spec table
enum WeaponType {
    WPN_NONE = 0,
    WPN_SOLAR_CORE,      // 10 slots, 95 dmg  — Artifact
    WPN_LUNAR_BLADE,     // 10 slots, 90 dmg  — Artifact
    WPN_IRON_HALBERD,    //  7 slots, 55 dmg
    WPN_VENOM_DAGGER,    //  4 slots, 30 dmg
    WPN_THUNDERSTAFF,    //  6 slots, 50 dmg
    WPN_OBSIDIAN_AXE,    //  5 slots, 45 dmg
    WPN_FROSTBOW,        //  6 slots, 48 dmg
    WPN_SPLINTER_STICK,  //  2 slots, 12 dmg
    WPN_ECLIPSE_RELIC    //  5 slots, 100 dmg — Dynamic artifact introduced at runtime
};

// ===== INVENTORY =====
struct Inventory {
    WeaponType slots[20];           // Primary: contiguous 20-slot linear array
    WeaponType long_term_storage[50]; // LTS for swapped-out weapons
    int lts_count;
};

// ===== ENTITY =====
struct Entity {
    int   id;
    bool  is_player;
    int   hp;
    int   max_hp;
    int   damage;
    int   speed;
    int   stamina;
    int   max_stamina;
    bool  is_alive;
    bool  is_stunned;
    pid_t pid;             // Process ID — for sending SIGUSR1 stun signals
    Inventory inv;
};

// ===== GLOBAL GAME STATE (shared memory segment) =====
struct GameState {
    // Custom Test-And-Set spinlock (replaces sem_t / pthread_mutex_t)
    // Built using hardware __sync_lock_test_and_set — the OS primitive taught in class
    volatile int custom_global_lock;

    bool setup_complete;  // Set by HIP after player selects party size
    int  num_players;     // 1-4
    int  num_enemies;     // 2-9, random per run

    Entity players[MAX_PLAYERS];
    Entity enemies[MAX_ENEMIES];

    bool game_running;
    bool asp_suspended;   // true = Ultimate ability active, ASP is SIGSTOP'd
    pid_t asp_pid;        // PID of the ASP process for SIGSTOP/SIGCONT

    // Turn state
    int  current_turn_id;         // Index into players[] or enemies[]
    bool current_turn_is_player;  // Which array current_turn_id refers to

    // Action buffer (written by HIP/ASP, read by Arbiter)
    bool       action_submitted;
    int        action_type;       // ACTION_* constant
    int        action_target_id;
    WeaponType action_weapon;

    // Artifact resource table
    int  solar_core_locked_by_id;
    bool solar_core_locked_by_player;
    int  lunar_blade_locked_by_id;
    bool lunar_blade_locked_by_player;
    bool eclipse_relic_in_world;   // True once it has been introduced

    // Win condition: kill 10 enemies total
    int total_kills;
    int win_kill_target; // = 10

    // Weapon drop (player must choose Y/N to pick up)
    bool      weapon_drop_pending;      // true = enemy just died, player must decide
    WeaponType weapon_drop_type;         // the weapon that dropped

    // PIDs of other processes (so HIP can kill them on quit)
    pid_t arbiter_pid;

    // Shared log message (last event description shown on UI)
    char log_message[256];
};

// ===== CUSTOM SPINLOCK (Test-And-Set algorithm) =====
// Uses hardware atomic instruction — NOT std::mutex / pthread_mutex / sem_t
// This is the memory-based synchronization primitive required by the course.
static inline void custom_lock_acquire(volatile int* lock) {
    // Spin until we atomically swap 0→1 (meaning we claimed the lock)
    while (__sync_lock_test_and_set(lock, 1)) {
        while (*lock) {
            usleep(10); // brief pause to reduce bus contention while spinning
        }
    }
}

static inline void custom_lock_release(volatile int* lock) {
    __sync_lock_release(lock); // atomic write of 0 (release)
}

// ===== WEAPON HELPER FUNCTIONS =====

static inline int get_weapon_size(WeaponType w) {
    switch (w) {
        case WPN_SOLAR_CORE:     return 10;
        case WPN_LUNAR_BLADE:    return 10;
        case WPN_IRON_HALBERD:   return 7;
        case WPN_VENOM_DAGGER:   return 4;
        case WPN_THUNDERSTAFF:   return 6;
        case WPN_OBSIDIAN_AXE:   return 5;
        case WPN_FROSTBOW:       return 6;
        case WPN_SPLINTER_STICK: return 2;
        case WPN_ECLIPSE_RELIC:  return 5;
        default:                 return 0;
    }
}

static inline int get_weapon_damage(WeaponType w) {
    switch (w) {
        case WPN_SOLAR_CORE:     return 95;
        case WPN_LUNAR_BLADE:    return 90;
        case WPN_IRON_HALBERD:   return 55;
        case WPN_VENOM_DAGGER:   return 30;
        case WPN_THUNDERSTAFF:   return 50;
        case WPN_OBSIDIAN_AXE:   return 45;
        case WPN_FROSTBOW:       return 48;
        case WPN_SPLINTER_STICK: return 12;
        case WPN_ECLIPSE_RELIC:  return 100;
        default:                 return 0;
    }
}

static inline const char* get_weapon_name(WeaponType w) {
    switch (w) {
        case WPN_SOLAR_CORE:     return "Solar Core";
        case WPN_LUNAR_BLADE:    return "Lunar Blade";
        case WPN_IRON_HALBERD:   return "Iron Halberd";
        case WPN_VENOM_DAGGER:   return "Venom Dagger";
        case WPN_THUNDERSTAFF:   return "Thunderstaff";
        case WPN_OBSIDIAN_AXE:   return "Obsidian Axe";
        case WPN_FROSTBOW:       return "Frostbow";
        case WPN_SPLINTER_STICK: return "Splinter Stick";
        case WPN_ECLIPSE_RELIC:  return "Eclipse Relic";
        default:                 return "None";
    }
}
