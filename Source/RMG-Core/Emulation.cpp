/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define CORE_INTERNAL
#include "MediaLoader.hpp"
#include "RomSettings.hpp"
#include "Emulation.hpp"
#include "RomHeader.hpp"
#include "Settings.hpp"
#include "Library.hpp"
#include "Netplay.hpp"
#include "Plugins.hpp"
#include "Cheats.hpp"
#include "Error.hpp"
#include "File.hpp"
#include "Rom.hpp"

#ifdef DISCORD_RPC
#include "DiscordRpc.hpp"
#endif // DISCORD_RPC

#include "m64p/Api.hpp"

// Constants for rollback netplay
#define CONTROLLER_COUNT 4      // Max number of controllers 
#define ROLLBACK_INPUT_BYTES 32  // Size of input data per player
#define ROLLBACK_VERBOSE false  // Set to true for verbose rollback logging
#define l_RollbackMaxPlayers 4  // Maximum number of supported players

// Frame callback function type definition
typedef void (*frame_callback_fn)(unsigned int);

// Frame callback data structure for rollback netplay
struct m64p_frame_callback_data {
    uint32_t frame_count;      // Current frame number
    uint32_t input_sequence;   // Input sequence number
    uint8_t* input_data;       // Pointer to input data
    bool is_rollback;          // Whether this frame is a rollback frame
    frame_callback_fn callback; // Function pointer for frame callback
};

//
// Local Functions
//

static bool get_emulation_state(m64p_emu_state* state)
{
    std::string error;
    m64p_error ret;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_CORE_STATE_QUERY, M64CORE_EMU_STATE, state);
    if (ret != M64ERR_SUCCESS)
    {
        error = "get_emulation_state m64p::Core.DoCommand(M64CMD_CORE_STATE_QUERY) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

static void apply_coresettings_overlay(void)
{
    CoreSettingsSetValue(SettingsID::Core_RandomizeInterrupt, CoreSettingsGetBoolValue(SettingsID::CoreOverlay_RandomizeInterrupt));
    CoreSettingsSetValue(SettingsID::Core_CPU_Emulator, CoreSettingsGetIntValue(SettingsID::CoreOverlay_CPU_Emulator));
    CoreSettingsSetValue(SettingsID::Core_DisableExtraMem, CoreSettingsGetBoolValue(SettingsID::CoreOverlay_DisableExtraMem));
    CoreSettingsSetValue(SettingsID::Core_EnableDebugger, CoreSettingsGetBoolValue(SettingsID::CoreOverlay_EnableDebugger));
    CoreSettingsSetValue(SettingsID::Core_CountPerOp, CoreSettingsGetIntValue(SettingsID::CoreOverlay_CountPerOp));
    CoreSettingsSetValue(SettingsID::Core_CountPerOpDenomPot, CoreSettingsGetIntValue(SettingsID::CoreOverlay_CountPerOpDenomPot));
    CoreSettingsSetValue(SettingsID::Core_SiDmaDuration, CoreSettingsGetIntValue(SettingsID::CoreOverlay_SiDmaDuration));
    CoreSettingsSetValue(SettingsID::Core_SaveFileNameFormat, CoreSettingsGetIntValue(SettingsID::CoreOverLay_SaveFileNameFormat));
}

static void apply_game_coresettings_overlay(void)
{
    std::string section;
    CoreRomSettings romSettings;
    bool overrideCoreSettings;

    // when we fail to retrieve the rom settings, return
    if (!CoreGetCurrentDefaultRomSettings(romSettings))
    {
        return;
    }

    section = romSettings.MD5;

    // when we don't need to override the core settings, return
    overrideCoreSettings = CoreSettingsGetBoolValue(SettingsID::Game_OverrideCoreSettings, section);
    if (!overrideCoreSettings)
    {
        return;
    }

    // apply settings overlay
    CoreSettingsSetValue(SettingsID::Core_RandomizeInterrupt, CoreSettingsGetBoolValue(SettingsID::Game_RandomizeInterrupt, section));
    CoreSettingsSetValue(SettingsID::Core_CPU_Emulator, CoreSettingsGetIntValue(SettingsID::Game_CPU_Emulator, section));
    CoreSettingsSetValue(SettingsID::Core_CountPerOpDenomPot, CoreSettingsGetIntValue(SettingsID::Game_CountPerOpDenomPot, section));
}

static void apply_pif_rom_settings(void)
{
    CoreRomHeader romHeader;
    std::string error;
    m64p_error ret;
    int cpuEmulator;
    bool usePifROM;

    // when we fail to retrieve the rom settings, return
    if (!CoreGetCurrentRomHeader(romHeader))
    {
        return;
    }

    // when we're using the dynarec, return
    cpuEmulator = CoreSettingsGetIntValue(SettingsID::Core_CPU_Emulator);
    if (cpuEmulator >= 2)
    {
        return;
    }

    usePifROM = CoreSettingsGetBoolValue(SettingsID::Core_PIF_Use);
    if (!usePifROM)
    {
        return;
    }

    const SettingsID settingsIds[] =
    {
        SettingsID::Core_PIF_NTSC,
        SettingsID::Core_PIF_PAL,
    };

    std::string rom = CoreSettingsGetStringValue(settingsIds[(int)romHeader.SystemType]);
    if (!std::filesystem::is_regular_file(rom))
    {
        return;
    }

    std::vector<char> buffer;
    if (!CoreReadFile(rom, buffer))
    {
        return;
    }

    ret = m64p::Core.DoCommand(M64CMD_PIF_OPEN, buffer.size(), buffer.data());
    if (ret != M64ERR_SUCCESS)
    {
        error = "open_pif_rom m64p::Core.DoCommand(M64CMD_PIF_OPEN) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }
}

// Add a new function to handle frame callbacks for rollback netplay
static m64p_frame_callback_data* frameCallbackData = nullptr;

// Variables to track inputs
uint32_t g_currentInputSequence = 0;
static uint32_t g_lastSavedFrameInputSequence = 0;
static uint8_t g_lastInputs[ROLLBACK_INPUT_BYTES * 4]; // Store last inputs for all 4 players

// Controller input structure - based on N64 controller layout
struct ControllerInput {
    // Digital buttons
    uint16_t buttons;     // A, B, Z, Start, DPad, Shoulder buttons
    // Analog stick
    int8_t  stick_x;      // -128 to 127
    int8_t  stick_y;      // -128 to 127
    // Triggers and extra data
    uint8_t trigger_r;    // Right trigger value
    uint8_t trigger_l;    // Left trigger value
    uint8_t reserved[2];  // Padding for future use
};

// Button bit definitions
#define BTN_A           (1<<0)
#define BTN_B           (1<<1)
#define BTN_Z           (1<<2)
#define BTN_START       (1<<3)
#define BTN_DPAD_UP     (1<<4)
#define BTN_DPAD_DOWN   (1<<5)
#define BTN_DPAD_LEFT   (1<<6)
#define BTN_DPAD_RIGHT  (1<<7)
#define BTN_SHOULDER_L  (1<<8)
#define BTN_SHOULDER_R  (1<<9)
#define BTN_C_UP        (1<<10)
#define BTN_C_DOWN      (1<<11)
#define BTN_C_LEFT      (1<<12)
#define BTN_C_RIGHT     (1<<13)

// M64P API Functions for controller input
namespace m64p {
    // Define input API functions for controller interaction
    extern "C" {
        struct Input_api 
        {
            bool (*GetStatus)(int ControllerNum, int* status);
            bool (*ReadController)(int ControllerNum, uint32_t* buttons, int8_t* x_axis, int8_t* y_axis);
            bool (*SetButtonState)(int ControllerNum, uint32_t buttons);
            bool (*SetAxisValue)(int ControllerNum, int axis, int8_t value);
        };
        extern Input_api Input;
    }
    
    // Define the actual Input variable
    Input_api Input = {0};
}

// Get controller inputs and pack into the provided buffer
static void GetControllerInputs(uint8_t* inputData) {
    if (!inputData) {
        return;
    }
    
    // Get inputs for the local player
    int playerIndex = CoreGetNetplayPlayerIndex();
    
    // Create and initialize a controller input structure
    ControllerInput input;
    memset(&input, 0, sizeof(input));
    
    // Get button states from mupen64plus core API
    int controllerConnected = 0;
    uint32_t buttons = 0;
    int8_t x_axis = 0;
    int8_t y_axis = 0;
    
    // Check if controller is connected via m64p API
    if (m64p::Input.GetStatus && 
        m64p::Input.GetStatus(playerIndex + 1, &controllerConnected) && 
        controllerConnected) {
        
        // Get controller state using the ReadController API if available
        if (m64p::Input.ReadController && 
            m64p::Input.ReadController(playerIndex + 1, &buttons, &x_axis, &y_axis)) {
            
            // Map the core's button states to our format
            if (buttons & 0x0001) input.buttons |= BTN_DPAD_RIGHT;  // R_DPAD
            if (buttons & 0x0002) input.buttons |= BTN_DPAD_LEFT;   // L_DPAD
            if (buttons & 0x0004) input.buttons |= BTN_DPAD_DOWN;   // D_DPAD
            if (buttons & 0x0008) input.buttons |= BTN_DPAD_UP;     // U_DPAD
            if (buttons & 0x0010) input.buttons |= BTN_START;       // START
            if (buttons & 0x0020) input.buttons |= BTN_Z;           // Z
            if (buttons & 0x0040) input.buttons |= BTN_B;           // B
            if (buttons & 0x0080) input.buttons |= BTN_A;           // A
            if (buttons & 0x0100) input.buttons |= BTN_SHOULDER_R;  // R_TRIG
            if (buttons & 0x0200) input.buttons |= BTN_SHOULDER_L;  // L_TRIG
            if (buttons & 0x0400) input.buttons |= BTN_C_RIGHT;     // R_CBUTTON
            if (buttons & 0x0800) input.buttons |= BTN_C_LEFT;      // L_CBUTTON
            if (buttons & 0x1000) input.buttons |= BTN_C_DOWN;      // D_CBUTTON
            if (buttons & 0x2000) input.buttons |= BTN_C_UP;        // U_CBUTTON
            
            // Convert to proper range for our structure (-128 to 127)
            input.stick_x = x_axis;
            input.stick_y = y_axis;
            
            // Set trigger values based on button states
            input.trigger_l = (buttons & 0x0200) ? 255 : 0; // L trigger
            input.trigger_r = (buttons & 0x0100) ? 255 : 0; // R trigger
        }
    }
    
    // Pack the input structure into the data buffer
    memcpy(&inputData[playerIndex * ROLLBACK_INPUT_BYTES], &input, sizeof(ControllerInput));
}

// Apply inputs to the emulator controllers
void ApplyControllerInputs(uint8_t* inputData, int playerIndex) {
    if (!inputData || playerIndex < 0 || playerIndex >= 4) {
        return;
    }
    
    // Extract the controller input data for this player
    ControllerInput input;
    memcpy(&input, &inputData[playerIndex * ROLLBACK_INPUT_BYTES], sizeof(ControllerInput));
    
    // Convert our buttons to the format expected by mupen64plus
    uint32_t buttons = 0;
    
    // Map our buttons to the core's format
    if (input.buttons & BTN_DPAD_RIGHT) buttons |= 0x0001;  // R_DPAD
    if (input.buttons & BTN_DPAD_LEFT)  buttons |= 0x0002;  // L_DPAD
    if (input.buttons & BTN_DPAD_DOWN)  buttons |= 0x0004;  // D_DPAD
    if (input.buttons & BTN_DPAD_UP)    buttons |= 0x0008;  // U_DPAD
    if (input.buttons & BTN_START)      buttons |= 0x0010;  // START
    if (input.buttons & BTN_Z)          buttons |= 0x0020;  // Z
    if (input.buttons & BTN_B)          buttons |= 0x0040;  // B
    if (input.buttons & BTN_A)          buttons |= 0x0080;  // A
    if (input.buttons & BTN_SHOULDER_R) buttons |= 0x0100;  // R_TRIG
    if (input.buttons & BTN_SHOULDER_L) buttons |= 0x0200;  // L_TRIG
    if (input.buttons & BTN_C_RIGHT)    buttons |= 0x0400;  // R_CBUTTON
    if (input.buttons & BTN_C_LEFT)     buttons |= 0x0800;  // L_CBUTTON
    if (input.buttons & BTN_C_DOWN)     buttons |= 0x1000;  // D_CBUTTON
    if (input.buttons & BTN_C_UP)       buttons |= 0x2000;  // U_CBUTTON
    
    // Apply to emulator via m64p API
    if (m64p::Input.SetButtonState) {
        m64p::Input.SetButtonState(playerIndex + 1, buttons);
    }
    
    // Set the analog stick positions if API is available
    if (m64p::Input.SetAxisValue) {
        m64p::Input.SetAxisValue(playerIndex + 1, 0, input.stick_x); // X-axis
        m64p::Input.SetAxisValue(playerIndex + 1, 1, input.stick_y); // Y-axis
    }
    
    // Store the inputs locally for later reference
    memcpy(&g_lastInputs[playerIndex * ROLLBACK_INPUT_BYTES], &input, sizeof(ControllerInput));
}

static void EmulationFrameCallback(unsigned int FrameIndex)
{
    // This will be called at the end of each video frame
    
    // Handle rollback netplay if active
    if (CoreHasInitRollbackNetplay())
    {
        // Get inputs from local controller
        uint8_t inputData[ROLLBACK_INPUT_BYTES * 4]; // Buffer for all players
        GetControllerInputs(inputData);
        
        // Increment input sequence for tracking
        g_currentInputSequence++;
        
        // Process frame using GGPO
        CoreRollbackNetplayAddLocalInput(inputData);
        
        // Store current input for potential use in state save/load
        memcpy(g_lastInputs, inputData, ROLLBACK_INPUT_BYTES);
        
        // Advance the frame in GGPO - this synchronizes with remote player
        // and possibly triggers rollbacks if needed
        CoreRollbackNetplayAdvanceFrame();
        
        // Get synchronized inputs from all players
        uint8_t allInputs[ROLLBACK_INPUT_BYTES * 4]; // Max 4 players
        if (CoreRollbackNetplayGetSynchronizedInputs(allInputs)) {
            // Apply inputs to all virtual controllers
            for (int i = 0; i < 4; i++) {
                ApplyControllerInputs(allInputs, i);
            }
        }
    }
}

//
// Exported Functions
//

CORE_EXPORT bool CoreStartEmulation(std::filesystem::path n64rom, std::filesystem::path n64ddrom, 
    std::string address, int port, int player)
{
    std::string error;
    m64p_error  m64p_ret;
    bool        netplay_ret = false;
    bool        rollback_netplay = false;
    bool        rollback = false;
    CoreRomType type;
    bool        netplay = !address.empty();

    if (!CoreOpenRom(n64rom))
    {
        return false;
    }

    if (!CoreApplyRomPluginSettings())
    {
        CoreApplyPluginSettings();
        CoreCloseRom();
        return false;
    }

    if (!CoreArePluginsReady())
    {
        CoreApplyPluginSettings();
        CoreCloseRom();
        return false;
    }

    if (!CoreAttachPlugins())
    {
        CoreApplyPluginSettings();
        CoreCloseRom();
        return false;
    }

    if (netplay)
    { // netplay cheats
        if (!CoreApplyNetplayCheats())
        {
            CoreDetachPlugins();
            CoreApplyPluginSettings();
            CoreCloseRom();
            return false;
        }
    }
    else
    { // local cheats
        if (!CoreApplyCheats())
        {
            CoreDetachPlugins();
            CoreApplyPluginSettings();
            CoreCloseRom();
            return false;
        }
    }

    if (!CoreGetRomType(type))
    {
        CoreClearCheats();
        CoreDetachPlugins();
        CoreApplyPluginSettings();
        CoreCloseRom();
        return false;
    }

    // set disk file in media loader when ROM is a cartridge
    if (type == CoreRomType::Cartridge)
    {
        CoreMediaLoaderSetDiskFile(n64ddrom);
    }

    // apply core settings overlay
    apply_coresettings_overlay();

    // apply game core settings overrides
    apply_game_coresettings_overlay();

    // apply pif rom settings
    apply_pif_rom_settings();

#ifdef DISCORD_RPC
    CoreDiscordRpcUpdate(true);
#endif // DISCORD_RPC

    // Check if we should use rollback netplay
    rollback = CoreSettingsGetBoolValue(SettingsID::Core_UseRollbackNetplay);
    
    // If rollback is enabled and netplay is requested, use rollback netplay
    rollback_netplay = rollback && netplay;

    if (netplay)
    {
        if (rollback_netplay)
        {
            // Initialize rollback netplay - default to 2 players for now
            int maxPlayers = 2;
            netplay_ret = CoreInitRollbackNetplay(address, port, player, maxPlayers);
            if (!netplay_ret)
            {
                CoreSetError("Failed to initialize rollback netplay");
                CoreStopEmulation();
                return false;
            }
            
            // Register frame callback for rollback
            frameCallbackData = new m64p_frame_callback_data();
            frameCallbackData->callback = EmulationFrameCallback;
            m64p::Core.DoCommand(M64CMD_SET_FRAME_CALLBACK, 0, frameCallbackData);
        }
        else
        {
            // Initialize traditional netplay
            netplay_ret = CoreInitNetplay(address, port, player);
            if (!netplay_ret)
            {
                CoreStopEmulation();
                return false;
            }
        }
    }

    // only start emulation when initializing netplay
    // is successful or if there's no netplay requested
    if (!netplay || netplay_ret)
    {
        m64p_ret = m64p::Core.DoCommand(M64CMD_EXECUTE, 0, nullptr);
        if (m64p_ret != M64ERR_SUCCESS)
        {
            error = "CoreStartEmulation m64p::Core.DoCommand(M64CMD_EXECUTE) Failed: ";
            error += m64p::Core.ErrorMessage(m64p_ret);
        }
    }

    CoreClearCheats();
    CoreDetachPlugins();
    CoreCloseRom();

    // restore plugin settings
    CoreApplyPluginSettings();

    // reset media loader state
    CoreResetMediaLoader();

#ifdef DISCORD_RPC
    CoreDiscordRpcUpdate(false);
#endif // DISCORD_RPC

    if (!netplay || netplay_ret)
    {
        // we need to set the emulation error last,
        // to prevent the other functions from
        // overriding the emulation error
        CoreSetError(error);
    }

    return m64p_ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreStopEmulation(void)
{
    std::string error;
    m64p_error ret;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_STOP, 0, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreStopEmulation m64p::Core.DoCommand(M64CMD_STOP) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
        return false;
    }

    // Add rollback netplay shutdown
    if (CoreHasInitRollbackNetplay())
    {
        CoreShutdownRollbackNetplay();
        
        // Clean up frame callback
        if (frameCallbackData)
        {
            delete frameCallbackData;
            frameCallbackData = nullptr;
        }
    }

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CorePauseEmulation(void)
{
    std::string error;
    m64p_error ret;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    if (CoreHasInitNetplay())
    {
        return false;
    }

    if (!CoreIsEmulationRunning())
    {
        error = "CorePauseEmulation Failed: ";
        error += "cannot pause emulation when emulation isn't running!";\
        CoreSetError(error);
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_PAUSE, 0, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CorePauseEmulation m64p::Core.DoCommand(M64CMD_PAUSE) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreResumeEmulation(void)
{
    std::string error;
    m64p_error ret;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    if (CoreHasInitNetplay())
    {
        return false;
    }

    if (!CoreIsEmulationPaused())
    {
        error = "CoreIsEmulationPaused Failed: ";
        error += "cannot resume emulation when emulation isn't paused!";
        CoreSetError(error);
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_RESUME, 0, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreResumeEmulation m64p::Core.DoCommand(M64CMD_RESUME) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreResetEmulation(bool hard)
{
    std::string error;
    m64p_error ret;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    if (CoreIsEmulationPaused())
    {
        error = "CoreResetEmulation Failed: ";
        error += "cannot reset emulation when paused!";
        CoreSetError(error);
        return false;
    }

    if (!CoreIsEmulationRunning())
    {
        error = "CoreResetEmulation Failed: ";
        error += "cannot reset emulation when emulation isn't running!";
        CoreSetError(error);
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_RESET, hard, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreResetEmulation m64p::Core.DoCommand(M64CMD_RESET) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreIsEmulationRunning(void)
{
    m64p_emu_state state = M64EMU_STOPPED;
    return get_emulation_state(&state) && state == M64EMU_RUNNING;
}

CORE_EXPORT bool CoreIsEmulationPaused(void)
{
    m64p_emu_state state = M64EMU_STOPPED;
    return get_emulation_state(&state) && state == M64EMU_PAUSED;
}
