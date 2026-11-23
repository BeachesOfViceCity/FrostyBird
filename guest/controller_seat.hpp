#pragma once
#include <windows.h>
#include <cstring>

// GameInput and the local GDK user service identify the same logical seats.
// Seat zero keeps the identifier used by existing local-player profiles.
inline APP_LOCAL_DEVICE_ID controller_seat_id(unsigned seat) {
    APP_LOCAL_DEVICE_ID id{};
    constexpr char name[]="Frosty local controller 0";
    static_assert(sizeof(name)<=sizeof(id.value));
    std::memcpy(id.value,name,sizeof(name)-1);
    id.value[sizeof(name)-2]=static_cast<BYTE>('0'+seat);
    return id;
}
