// app_install.h - the small things that make the install one step: finding the player's disc image whatever they named it, and
// putting the bubble's cover and background where the RetroFlow launcher looks for them.
#pragma once

#include <string>

namespace app {

// The name, inside the data folder (ux0:data/infected/), of the disc image to use. "Infected.iso" when that file is there. Otherwise
// the .iso that looks like it (a name with "infected" or "10054" in it first, else the first one by name), so a dump named
// "Infected (USA).iso" works without renaming. Only files of 64 MiB or more count. With none, "Infected.iso": the check then says
// where to put the file.
std::string find_disc_name();

// Copies the cover and the background the package carries (app0:retroflow/) to ux0:data/RetroFlow/COVERS|BACKGROUNDS/Homebrew/<id>.png.
// Only when RetroFlow is installed (its data folder exists) and only for a picture that is not there: art the player put there stays.
// Cheap when there is nothing to do (a few file lookups), so it runs at every start and catches a RetroFlow installed later.
void install_retroflow_art();

}  // namespace app
