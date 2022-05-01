# Motorsport Manager Practice Driver Fixer

The Motorsport Manager (MM) game has a bug which prevents some drivers being selected for practice sessions. The bug can be fixed by modifying up to three incorrect values in the MM save file. [Credit to the original poster of this fix](https://steamcommunity.com/app/415200/discussions/2/1482109512320472245/?ctp=2#c3561682880008852087 "https://steamcommunity.com"). This program allows anoyone to open the save file, fix the incorrect values, then save a fixed copy of the file with minimal effort.

## Instructions

1. Download the latest release **[from here](https://github.com/sieve-mind/mm-practice-driver-fixer/releases/latest "from here")**, unzip it, and run MMPracticeDriverFixer.exe
   - Or download the source code and build the executable
2. If Motorsport Manager is running save your game and exit
3. Click the **Open Motorsport Manager Save File...** button and pick a save file
4. Select which driver should be in car 1 (purple), which driver should be in car 2 (orange),  and which driver should be in reserve. Make sure none of the drivers overlap

#### Example before

![edit](https://user-images.githubusercontent.com/104565115/166144927-6c0e316e-ce21-4199-ba5a-7a4d0441f58a.png)

#### Example after

![edit2](https://user-images.githubusercontent.com/104565115/166144930-c34d8b41-55e4-40ba-9a2a-5037e649f7bb.png)

If none over the drivers overlap when you open the save file then you have a different glitch which this program will be unable to fix.

5. Click the **Save Changes As...** button and save a new file
   - You can overwrite an existing save, but for the sake of safety I would not recommend it
6. Open Motorsport Manager and load the new save file, the glitch should now be fixed

## Disclaimer

Motorsport Manager Practice Driver Fixer is deigned to fix a specific glitch that I encountered while playing. It is not designed to fix any other problems with MM save files. I have a limited set of save files to test with. If the fixer is unable to read your save file but Motorsport Manager is then please open an issue and attach your save file.

## Supported Platforms

- Windows
  - Windows 10
  - Windows 11 (untested)
