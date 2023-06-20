# Alien Invasion Game
===================

This Python project is a simple 2D spaceship shooting game. The spaceship is controlled by the player to move left or right and shoot the invading aliens. The aliens also have the ability to shoot back at the spaceship.

The game is built using the Pygame library, and its structure is mainly composed of three classes: `Spaceship`, `Alien`, and `Bullet`.

## Features
--------

-   Spaceship and aliens have shooting capabilities.
-   Aliens move randomly within the screen and also randomly shoot bullets.
-   Spaceship can move left or right and shoot upwards.
-   Both spaceship and aliens can take multiple hits (max hits are 3) before they are destroyed.
-   Collisions are checked for each frame and the game reacts accordingly.
-   The game runs at a constant framerate of 60 FPS.

## Prerequisites
-------------

You need to have Python 3 and Pygame installed on your machine. If Pygame is not installed, you can install it using pip:

```bash

`pip install pygame`

```

## How to Run
----------

1.  Clone the repository or download the files.
2.  Run `main.py`.

## Controls
--------

-   `LEFT Arrow Key` - Moves the spaceship to the left.
-   `RIGHT Arrow Key` - Moves the spaceship to the right.
-   `SPACE Key` - Fires a bullet from the spaceship.
-   `ESC Key` - Exits the game.

## Files and Classes
-----------------

The main classes are:

-   `Spaceship` - Represents the spaceship controlled by the player. It can move left or right, shoot bullets, and handle collisions.
-   `Alien` - Represents the alien invaders. They move randomly, shoot bullets, and handle collisions.
-   `Bullet` - Represents the bullets shot by both the spaceship and the aliens.

The assets used in the game (spaceship and alien sprites) need to be placed in a directory named `sprites` in the same directory as the Python file.

Note: The paths for the images are hardcoded in the script so make sure the images are in the correct path or update the path in the script as per your folder structure.

## Customization
-------------

You can customize various aspects of the game like the spaceship speed, alien speed, bullet size, and the maximum hits before a spaceship or an alien is destroyed by changing the corresponding constants at the top of the `main.py` file.

## Limitations
-----------

-   There are no win or lose conditions defined, the game runs in an infinite loop.
-   The aliens do not respawn after they are destroyed.
-   There is no score tracking implemented.
