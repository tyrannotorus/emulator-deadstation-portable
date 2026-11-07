#pragma once

// The Brick's CPU clock while a game runs: Options' CPU Freq. 1.8GHz (the default) holds it at the top of NextUI's Auto range
// (scaling_min_freq raised to scaling_max_freq) while the game runs, and gives it back in the menu and at the end; Dynamic leaves
// it to NextUI's governor (schedutil), which sees the emulator's threads taking turns as idle cores and can settle far lower than a
// game needs (LEGO Indiana Jones at 600-1008 MHz, 59% speed; God of War's flips stalling 52 ms). Left alone when something else
// moves the floor (a test's pin), and with DEADSTATION_PORTABLE_CPUFREQ=dynamic.
namespace deadstation::cpufreq
{

void start();
// CPU Freq from the settings (settings::apply)
void setFixed(bool fixed);
// each host frame while the game runs
void tick();
// back to NextUI's floor: the menu, a pause
void release();
void stop();

}
