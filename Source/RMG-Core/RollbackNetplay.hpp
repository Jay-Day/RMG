/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_ROLLBACK_NETPLAY_HPP
#define CORE_ROLLBACK_NETPLAY_HPP

#include "m64p/Api.hpp"
#include <string>
#include <vector>
#include <memory>

#ifdef __cplusplus
extern "C" {
#endif
#include "ggponet.h"
#ifdef __cplusplus
}
#endif

// Default number of frames to predict ahead
#define ROLLBACK_MAX_PREDICTION_FRAMES 8

// Number of bytes per player input - must be enough for ControllerInput structure
#define ROLLBACK_INPUT_BYTES 32

// Forward declaration
class RollbackNetplayImpl;

// Main rollback netplay class
class RollbackNetplay 
{
public:
    // Rollback metrics structure for visualizing rollback behavior
    struct RollbackMetrics {
        int rollbackFrames;          // Number of frames rolled back
        int totalRollbacks;          // Total number of rollback events
        int predictedFrames;         // Frames that used predicted input
        int maxRollbackFrames;       // Maximum rollback distance
        float avgRollbackFrames;     // Average rollback distance
        int pingMs;                  // Current ping in milliseconds
        int remoteFrameAdvantage;    // Frame advantage of remote player
        
        // Reset metrics to default values
        void Reset() {
            rollbackFrames = 0;
            totalRollbacks = 0;
            predictedFrames = 0;
            maxRollbackFrames = 0;
            avgRollbackFrames = 0.0f;
            pingMs = 0;
            remoteFrameAdvantage = 0;
        }
    };

    // Constructor/Destructor
    RollbackNetplay();
    ~RollbackNetplay();

    // Initialize rollback netplay
    // address: IP address of remote player
    // port: Local port to use
    // player: Local player number (1-4)
    // maxPlayers: Total number of players
    // frameDelay: Number of frames to delay input (reduces rollbacks at cost of input lag)
    bool Initialize(const std::string& address, int port, int player, int maxPlayers, int frameDelay = 1);

    // Shutdown rollback netplay
    void Shutdown();

    // Returns whether rollback netplay is initialized
    bool IsInitialized() const;
    
    // Add local input to the system
    // input: Controller input data from the emulator
    // Returns true if inputs were accepted
    bool AddLocalInput(const uint8_t* input);
    
    // Get synchronized inputs for current frame
    // inputs: Array to store input data for all players
    // Returns true if inputs are valid
    bool GetSynchronizedInputs(uint8_t* inputs);
    
    // Advance the frame in GGPO
    bool AdvanceFrame();
    
    // Set emulation callback functions
    // saveState: Function to save emulator state
    // loadState: Function to load emulator state
    // freeState: Function to free allocated state memory
    // advanceFrame: Function to advance emulation frame
    void SetCallbacks(
        bool (*saveState)(void** buffer, int* len, int* checksum, int frame), 
        bool (*loadState)(void* buffer, int len),
        void (*freeState)(void* buffer),
        bool (*advanceFrame)());

    // Get current rollback metrics
    RollbackMetrics GetMetrics() const;
    
    // Check if a rollback just occurred (for visual effect)
    bool RollbackJustOccurred();

private:
    // Pimpl idiom to hide implementation details
    std::unique_ptr<RollbackNetplayImpl> impl;
};

#endif // CORE_ROLLBACK_NETPLAY_HPP 