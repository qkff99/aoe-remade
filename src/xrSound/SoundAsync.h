#pragma once

class CSound_manager_interface;
class CSoundRender_CoreA;

// The facade owns gameplay references; its backend owns only audio state.
CSound_manager_interface* CreateAsyncSound(CSoundRender_CoreA* backend);
