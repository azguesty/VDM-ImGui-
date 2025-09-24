#pragma once
#ifndef THEME_H
#define THEME_H

#include <imgui.h>

// Theme management for VDM
class Theme {
private:
    static bool s_useDarkTheme;

public:
    // Initialize theme system (call once at startup)
    static void initialize();

    // Get current theme state
    static bool isDarkTheme() { return s_useDarkTheme; }

    // Set theme and apply it
    static void setDarkTheme(bool useDark);

    // Apply current theme settings
    static void apply();

    // Get background clear color for current theme
    static ImVec4 getBackgroundColor();
    static ImVec4 getTextColor();

private:
    // Apply dark theme colors
    static void applyDarkTheme();

    // Apply light theme colors  
    static void applyLightTheme();

    // Apply common style settings
    static void applyCommonStyle();
};

#endif // THEME_H