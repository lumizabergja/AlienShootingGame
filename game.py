import pygame
import random
import sys

SCREEN_WIDTH = 640
SCREEN_HEIGHT = 480
SPACESHIP_SPEED = 5
SPACESHIP_WIDTH = 60
ALIEN_WIDTH = 40
BULLET_SIZE = 5
MAX_HITS = 3

class Spaceship:
    def __init__(self, window):
        self.alien_kills = 0
        self.x = SCREEN_WIDTH / 2
        self.y = 400
        self.vel_x = 0
        self.window = window
        self.bullets = []  # List to store bullets
        self.hits = 0  # Number of hits on the spaceship
        self.spaceship_image = self.load_spaceship()
        self.rect = pygame.Rect(self.x, self.y, SPACESHIP_WIDTH, self.load_spaceship().get_height())


    def move(self):
        new_x = self.x + self.vel_x
        if new_x >= 0 and new_x <= SCREEN_WIDTH - self.load_spaceship().get_width():
            self.x = new_x
        elif new_x < 0:
            self.x = 0
        elif new_x > SCREEN_WIDTH - self.load_spaceship().get_width():
            self.x = SCREEN_WIDTH - self.load_spaceship().get_width()

        self.rect.x = self.x

    def shoot(self):
        bullet = Bullet(self.x + self.load_spaceship().get_width() // 2 - 2, self.y)
        bullet.bullet_speed = -10  # Set the bullet speed to move upward
        self.bullets.append(bullet)

    def load_spaceship(self):
        spaceship_image = loadify("sprites/space_ship.png")
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
            bullet.draw(self.window, (255, 0, 0))  # Set the color to red (RGB: 255, 0, 0)

    def check_bullet_collision(self, aliens):
        for bullet in self.bullets:
            for alien in aliens:
                if bullet.collides_with(alien):
                    alien.hit()
                    self.bullets.remove(bullet)
                    break

    def hit(self):
        self.hits += 1
        if self.hits >= MAX_HITS:
            game_over_screen(self.window, self.alien_kills)

class Alien:
    def __init__(self, x, y, speed):
        self.x = x
        self.y = y
        self.speed = speed
        self.direction_x = random.choice([-1, 1])
        self.direction_y = random.choice([-1, 1])
        self.hits = 0
        self.shoot_timer = random.randint(0, 10)
        self.bullets = []
        self.is_hit = False  # Flag to track if the alien is hit
        self.is_dead = False  # Flag to track if the alien is dead
        self.alien_ok_image = self.load_alien_ok()
        self.alien_hit_image = self.load_alien_hit()
        self.alien_dead_image = self.load_alien_dead()
        self.rect = pygame.Rect(self.x, self.y, ALIEN_WIDTH, ALIEN_WIDTH)
        
    def check_bullet_collision(self, spaceship):
        for bullet in self.bullets:
            if bullet.collides_with(spaceship):
                spaceship.hit()
                self.bullets.remove(bullet)
                break
    
    def load_alien_ok(self):
        return self.draw_alien("sprites/alien_ok.png")

    def load_alien_hit(self):
        return self.draw_alien("sprites/alien_hit.png")

    def load_alien_dead(self):
        return self.draw_alien("sprites/alien_dead.png")


    def move(self):
        if not self.is_dead:
            self.x += self.speed * self.direction_x
            self.y += self.speed * self.direction_y
            self.rect.x = self.x
            self.rect.y = self.y
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
            elif self.hits >= 3:
                self.y += 10  # Move the dead alien downward

    def update(self):
        if self.hits < MAX_HITS:
            self.move()
            self.shoot_timer = random.randint(0,150)
            if self.shoot_timer <= 0:
                self.shoot_bullet()
                self.shoot_timer = random.randint(0, 150)
        elif self.hits >= MAX_HITS:
            if self.y < SCREEN_HEIGHT:
                self.y += 5  # Move the dead alien downward
            else:
                self.is_dead = True  # Remove the dead alien from the list

    def shoot_bullet(self):
        if not self.is_dead:
            bullet = Bullet(self.x + ALIEN_WIDTH // 2 - BULLET_SIZE // 2, self.y + ALIEN_WIDTH)
            bullet.bullet_speed = 10  # Set the bullet speed to move downward
            self.bullets.append(bullet)

    def update_bullets(self):
        for bullet in self.bullets:
            bullet.update()
            if bullet.y > SCREEN_HEIGHT:
                self.bullets.remove(bullet)

    def draw_bullets(self, surface):
        for bullet in self.bullets:
            bullet.draw(surface, (0, 255, 0))  # Set the color to green (RGB: 0, 255, 0)

    def hit(self):
        self.hits += 1
        self.is_hit = True

    def draw_alien(self, filename):
        alien_image = loadify(filename)

        original_width, original_height = alien_image.get_size()
        alien_height = int(ALIEN_WIDTH * original_height / original_width)
        alien_image = pygame.transform.scale(alien_image, (ALIEN_WIDTH, alien_height))

        return alien_image

    def load_alien(self):
        if self.hits >= MAX_HITS:
            alien_image = self.alien_dead_image
        elif self.is_hit:
            alien_image = self.alien_hit_image
        else:
            alien_image = self.alien_ok_image

        return alien_image
    
class Bullet:
    def __init__(self, x, y):
        self.x = x
        self.y = y
        self.size = BULLET_SIZE  # Size of the square bullet
        self.bullet_speed = 20
        self.rect = pygame.Rect(self.x, self.y, self.size, self.size)

    def update(self):
        self.y += self.bullet_speed
        self.rect.y = self.y

    def draw(self, surface, color):
        pygame.draw.rect(surface, color, (self.x, self.y, self.size, self.size))

    def collides_with(self, entity):
        if isinstance(entity, Alien):
            return self.rect.colliderect(entity.rect)
        if isinstance(entity, Spaceship):
            return self.rect.colliderect(entity.rect)
    
def loadify(imgname):
    return pygame.image.load(imgname).convert_alpha()

def game_over_screen(window, alien_kills):
    pygame.init() 
    restart = False
    while not restart:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                pygame.quit()
                sys.exit()
            elif event.key == pygame.K_ESCAPE:
                    exit()
            elif event.type == pygame.KEYDOWN:
                if event.key == pygame.K_RETURN:
                    restart = True

        window.fill((0, 0, 0))  # Clear the window
        font = pygame.font.Font(None, 72)
        text = font.render("Game Over", True, (255, 255, 255))
        text_rect = text.get_rect(center=(SCREEN_WIDTH // 2, SCREEN_HEIGHT // 2))
        window.blit(text, text_rect)

        score_text = font.render(f"Alien Kills: {alien_kills}", True, (255, 255, 255))
        score_rect = score_text.get_rect(center=(SCREEN_WIDTH // 2, SCREEN_HEIGHT // 2 + 100))
        window.blit(score_text, score_rect)

        restart_text = font.render("Press Enter to Restart", True, (255, 255, 255))
        restart_rect = restart_text.get_rect(center=(SCREEN_WIDTH // 2, SCREEN_HEIGHT // 2 + 200))
        window.blit(restart_text, restart_rect)

        pygame.display.flip()

    run_game()

def run_game():
    pygame.init()
    window = pygame.display.set_mode((SCREEN_WIDTH, SCREEN_HEIGHT))
    spaceship = Spaceship(window)
    aliens = [Alien(random.randint(40, 600), random.randint(40, 200), 1) for _ in range(10)]
    clock = pygame.time.Clock()
    background_image = loadify("sprites/background.png")
    background_image = pygame.transform.scale(background_image, (SCREEN_WIDTH, SCREEN_HEIGHT))


    while True:
        spaceship.check_events()
        spaceship.move()
        spaceship.update_bullets()
        spaceship.check_bullet_collision(aliens)

        window.blit(background_image, (0, 0))

        for alien in aliens:
            alien.update()
            alien.update_bullets()
            alien.check_bullet_collision(spaceship)
            alien.draw_bullets(window)
            alien_surface = alien.load_alien()
            window.blit(alien_surface, (alien.x, alien.y))

            if alien.is_dead and alien.y >= SCREEN_HEIGHT:
                aliens.remove(alien)
                spaceship.alien_kills += 1
                new_alien = Alien(random.randint(40, 600), 40, 1)
                aliens.append(new_alien)

        space_ship = spaceship.load_spaceship()
        window.blit(space_ship, (spaceship.x, spaceship.y))
        spaceship.draw_bullets()

        # Display alien kills counter
        font = pygame.font.Font(None, 36)
        text = font.render(f"Alien Kills: {spaceship.alien_kills}", True, (255, 255, 255))
        text_rect = text.get_rect()
        text_rect.topright = (SCREEN_WIDTH - 10, 10)
        window.blit(text, text_rect)

        if spaceship.hits >= MAX_HITS:
            break

        pygame.display.flip()
        clock.tick(60)
    pygame.quit()
    game_over_screen(window, spaceship.alien_kills)

run_game()