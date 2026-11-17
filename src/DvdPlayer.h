#pragma once

#include <string>

class ScreenContextRgbSDL;

/**
 * Plays a DVD-Video folder (VIDEO_TS) the way a DVD player does: from its First Play program chain through its
 * menus and titles, with menu buttons chosen by the arrow keys and Enter or the mouse.
 * @param skipIntro skip the warnings, logos and trailers before the first menu (see Dvd::IntroSkipper)
 * @return the process exit code
 */
int playDvd(ScreenContextRgbSDL &sdl, const std::string &path, bool skipIntro = true);
