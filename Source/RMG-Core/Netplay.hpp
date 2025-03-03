/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_NETPLAY_HPP
#define CORE_NETPLAY_HPP

#include <string>
#include <cstdint>

// Forward declarations
namespace m64p {
    extern "C" {
        struct Input_api;
        extern Input_api Input;
    }
}

// Attempts to initialize netplay
bool CoreInitNetplay(std::string address, int port, int player);

// Returns whether netplay has been initialized
bool CoreHasInitNetplay(void);

// Attempts to shutdown netplay
bool CoreShutdownNetplay(void);

// Attempts to initialize rollback netplay (GGPO-based)
bool CoreInitRollbackNetplay(std::string address, int port, int player, int maxPlayers);

// Returns whether rollback netplay has been initialized
bool CoreHasInitRollbackNetplay(void);

// Attempts to shutdown rollback netplay
bool CoreShutdownRollbackNetplay(void);

// Process network events and advance the GGPO frame when using rollback netplay
bool CoreRollbackNetplayAdvanceFrame(void);

// Adds local controller inputs to the rollback system
bool CoreRollbackNetplayAddLocalInput(const uint8_t* input);

// Retrieves synchronized inputs for all players
bool CoreRollbackNetplayGetSynchronizedInputs(uint8_t* inputs);

// Apply the synchronized inputs to all virtual controllers
bool CoreRollbackNetplayApplyInputs(void);

// Get if rollbacks have occurred in the rollback netplay session
bool CoreRollbackNetplayHasRollbacks(void);

// Check if rollback just occurred (for visual indication)
bool CoreRollbackNetplayJustOccurred(void);

// Get the local player index (0-based) for rollback netplay
int CoreGetNetplayPlayerIndex(void);

// Get current rollback netplay metrics
bool CoreRollbackNetplayGetMetrics(int* rollbackFrames, int* totalRollbacks, int* predictedFrames,
                                 int* maxRollbackFrames, float* avgRollbackFrames,
                                 int* pingMs, int* remoteFrameAdvantage);

#endif // CORE_NETPLAY_HPP