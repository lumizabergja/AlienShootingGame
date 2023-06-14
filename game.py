import pygame
import random

SCREEN_WIDTH = 640
SCREEN_HEIGHT = 480
SPACESHIP_SPEED = 5
SPACESHIP_WIDTH = 60
ALIEN_WIDTH = 40

class Spaceship:
    def __init__(self, window):
        self.x = SCREEN_WIDTH / 2
        self.y = 400
        self.vel_x = 0
        self.window = window
        self.bullets = []  # List to store bullets

    def move(self):
        new_x = self.x + self.vel_x
        if new_x >= 0 and new_x <= SCREEN_WIDTH - self.load_spaceship().get_width():
            self.x = new_x
        elif new_x < 0:
            self.x = 0
        elif new_x > SCREEN_WIDTH - self.load_spaceship().get_width():
            self.x = SCREEN_WIDTH - self.load_spaceship().get_width()

    def shoot(self):
        bullet = Bullet(self.x + self.load_spaceship().get_width()/2 - 2, self.y)
        self.bullets.append(bullet)  # Add the bullet to the list

    def load_spaceship(self):
        spaceship_image = pygame.image.load("sprites/space_ship.png")
        original_width, original_height = spaceship_image.get_size()
        spaceship_height = int(SPACESHIP_WIDTH * original_height / original_width)
        spaceship_image = pygame.transform.scale(spaceship_image, (SPACESHIP_WIDTH, spaceship_height))
        return spaceship_image

    def check_events(self):
        for event in pygame.event.get():
            if event.type == pygame.KEYDOWN:
                if event.key == pygame.K_LEFT:
                    self.vel_x = -SPACESHIP_SPEED
                elif event.key == pygame.K_RIGHT:
                    self.vel_x = SPACESHIP_SPEED
                elif event.key == pygame.K_SPACE:
                    self.shoot()
                elif event.key == pygame.K_ESCAPE:
                    exit()
            elif event.type == pygame.KEYUP:
                if event.key == pygame.K_LEFT and self.vel_x < 0:
                    self.vel_x = 0
                elif event.key == pygame.K_RIGHT and self.vel_x > 0:
                    self.vel_x = 0
            elif event.type == pygame.QUIT:
                exit()

    def update_bullets(self):
        for bullet in self.bullets:
            bullet.update()
            if bullet.y < 0:  # Remove bullets that have gone off-screen
                self.bullets.remove(bullet)

    def draw_bullets(self):
        for bullet in self.bullets:
            bullet.draw(self.window)




class Bullet:
    def __init__(self, x, y):
        self.x = x
        self.y = y
        self.size = 5  # Size of the square bullet
        self.bullet_speed = 20

    def update(self):
        self.y -= self.bullet_speed
        
    def draw(self, surface):
        pygame.draw.rect(surface, (255, 0, 0), (self.x, self.y, self.size, self.size))



class Alien:
    def __init__(self,x ,y, speed):
        self.x = x
        self.y = y
        self.speed = speed
        self.direction_x = random.choice([-1, 1])
        self.direction_y = random.choice([-1, 1])

    def move(self):
        self.x += self.speed * self.direction_x
        self.y += self.speed * self.direction_y

        # Check if the alien reaches the horizontal window boundaries
        if self.x <= 0:
            self.x = 0
            self.direction_x = 1  # Change horizontal direction to move right
        elif self.x >= SCREEN_WIDTH - ALIEN_WIDTH:
            self.x = SCREEN_WIDTH - ALIEN_WIDTH
            self.direction_x = -1  # Change horizontal direction to move left

        # Check if the alien reaches the vertical range boundaries
        if self.y <= 40:
            self.y = 40
            self.direction_y = 1  # Change vertical direction to move down
        elif self.y >= 200:
            self.y = 200
            self.direction_y = -1  # Change vertical direction to move up

    def load_alien(self):
        alien_image = pygame.image.load("sprites/alien_ok.png")
        original_width, original_height = alien_image.get_size()
        alien_height = int(ALIEN_WIDTH * original_height / original_width)
        alien_image = pygame.transform.scale(alien_image, (ALIEN_WIDTH, alien_height))
        return alien_image
    

pygame.init()
window = pygame.display.set_mode((SCREEN_WIDTH, SCREEN_HEIGHT))
spaceship = Spaceship(window)
aliens = [Alien(random.randint(40, 600), random.randint(40, 200), 1) for _ in range(10)]
clock = pygame.time.Clock()

while True:
    spaceship.check_events()
    spaceship.move()

    window.fill((0, 0, 0))
    space_ship = spaceship.load_spaceship()
    window.blit(space_ship, (spaceship.x, spaceship.y))
    for alien in aliens:
        window.blit(alien.load_alien(), (alien.x, alien.y))
        alien.move()
    spaceship.update_bullets()  # Update and remove off-screen bullets
    spaceship.draw_bullets()  # Draw all bullets
    pygame.display.flip()
    clock.tick(60)