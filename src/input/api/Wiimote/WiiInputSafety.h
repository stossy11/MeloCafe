#pragma once

// Neutralize only the snapshot delivered to the game. Keep extension identity,
// calibration, fusion state and connection alive for the next fresh sample.
template<class State>
void NeutralizeWiiInput(State& state)
{
    state.buttons = 0;
    state.ir_camera.dots = {};
    state.ir_camera.prev_dots = {};
    state.ir_camera.position = {};
    state.ir_camera.m_prev_position = {};
    state.ir_camera.m_positionVisibility = PositionVisibility::NONE;
    state.motion_sample = state.motion_sample.stationary();
    if (state.m_motion_plus) state.m_motion_plus->angular_velocity = {};
    if (auto* nunchuck = std::get_if<NunchuckData>(&state.m_extension)) {
        nunchuck->c = nunchuck->z = false;
        nunchuck->axis = {};
        nunchuck->motion_sample = nunchuck->motion_sample.stationary();
    }
    if (auto* classic = std::get_if<ClassicData>(&state.m_extension)) {
        classic->buttons = 0;
        classic->left_axis = classic->right_axis = classic->trigger = {};
    }
}
