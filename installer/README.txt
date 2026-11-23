FROSTY BIRD   /   THE MATRIX AWAKENS
Linux x86-64 • Vulkan • Xbox Series X|S

Extract this entire ZIP. Open frostybird inside the FrostyBird folder.
If your archive manager dropped Unix permissions, extract with unzip instead.
Choose your original game package (the parent of Mount and EmbeddedXvd), or
drop its Mount folder on the Game package screen. Keep the package in place.

Setup installs for the current user, checks the files, configures movie audio
and video, and precompiles the recorded shaders and pipelines on your machine.
The first install takes longer than subsequent starts. When it finishes the
game opens automatically. Start it again from the Frosty Bird applications
entry, ~/.local/bin/frostybird, or the original frostybird executable.

M toggles the installer soundtrack and electronic menu cues. Tab / Enter, arrows, mouse and folder
drag-and-drop work in setup. --preview tours the interface without installing.
--setup reopens the installer to update from a newer ZIP or to select a moved
game folder. Run it from the newly extracted folder when updating.
--mute starts play
without audio. --help lists command-line options.

Requires a recent glibc-based x86-64 Linux desktop, a Vulkan driver with
hardware ray tracing, and your original Xbox Series game files. No compiler,
Python, Steam, SDK, root privileges or install-time download is required.
New scenes, previously unseen shader variants, driver changes and asset
streaming can still cause hitches.

Files: ~/.local/share/frostybird (runtime, prefix, player data and logs);
~/.cache/frostybird (regenerable shader caches). XDG locations are honored.
