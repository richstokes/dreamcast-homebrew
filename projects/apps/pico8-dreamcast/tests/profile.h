#pragma once
#ifdef DC_PROFILE
void dc_profile_begin();
void dc_profile_end(const char* cart);
#else
inline void dc_profile_begin() {}
inline void dc_profile_end(const char*) {}
#endif
