#include "raylib.h"
#include "raymath.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstring>

#define SCREEN_WIDTH  1000
#define SCREEN_HEIGHT 1000
#define WINDOW_TITLE  "Coal Alarm!"

#if defined(PLATFORM_DESKTOP)
    #define GLSL_VERSION            330
#else   // PLATFORM_ANDROID, PLATFORM_WEB
    #define GLSL_VERSION            100
#endif

#define BLOOM_SAMPLES  16
#define BLOOM_STRENGTH 0.1f  

#if defined(PLATFORM_DESKTOP)
static const char* bloomFragShaderSrc = R"(
#version 330
in vec2 fragTexCoord;
out vec4 finalColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform float threshold;
uniform float intensity;
uniform vec2 texelSize;
uniform int   bloomSamples;
uniform float bloomStrength;
void main() {
    vec4 c = texture(texture0, fragTexCoord);
    vec4 bloom = vec4(0.0);
    float total = float((2 * bloomSamples + 1) * (2 * bloomSamples + 1));
    for (int x = -bloomSamples; x <= bloomSamples; x++) {
        for (int y = -bloomSamples; y <= bloomSamples; y++) {
            vec2 off = vec2(float(x), float(y)) * texelSize * bloomStrength;
            vec4 s = texture(texture0, fragTexCoord + off);
            float bright = dot(s.rgb, vec3(0.299, 0.587, 0.114));
            bloom += s * max(bright - threshold, 0.0);
        }
    }
    bloom /= total;
    finalColor = min(c + bloom * intensity, vec4(1.0));
    finalColor.a = c.a;
}
)";
#else
static const char* bloomFragShaderSrc = R"(
#version 100
precision mediump float;
#define BLOOM_N 16
varying vec2 fragTexCoord;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform float threshold;
uniform float intensity;
uniform vec2 texelSize;
uniform float bloomStrength;
void main() {
    vec4 c = texture2D(texture0, fragTexCoord);
    vec4 bloom = vec4(0.0);
    float total = float((2 * BLOOM_N + 1) * (2 * BLOOM_N + 1));
    for (int x = -BLOOM_N; x <= BLOOM_N; x++) {
        for (int y = -BLOOM_N; y <= BLOOM_N; y++) {
            vec2 off = vec2(float(x), float(y)) * texelSize * bloomStrength;
            vec4 s = texture2D(texture0, fragTexCoord + off);
            float bright = dot(s.rgb, vec3(0.299, 0.587, 0.114));
            bloom += s * max(bright - threshold, 0.0);
        }
    }
    bloom /= total;
    gl_FragColor = min(c + bloom * intensity, vec4(1.0));
    gl_FragColor.a = c.a;
}
)";
#endif

int gamepad = -1;

static const float PLAYER_SIZE  = 50.0f;
static const float PLAYER_SPEED = 220.0f;

static Vector2 ResolveWall(Vector2 pos, float half, Rectangle wall) {
    float left   = pos.x - half,  right  = pos.x + half;
    float top    = pos.y - half,  bottom = pos.y + half;
    float wallLeft  = wall.x,              wallRight  = wall.x + wall.width;
    float wallTop   = wall.y,              wallBottom = wall.y + wall.height;

    if (right <= wallLeft || left >= wallRight || bottom <= wallTop || top >= wallBottom) return pos;

    float overlapLeft   = right  - wallLeft;
    float overlapRight  = wallRight  - left;
    float overlapTop    = bottom - wallTop;
    float overlapBottom = wallBottom - top;

    float minOverlap = overlapLeft;  Vector2 push = { -overlapLeft,  0             };
    if (overlapRight  < minOverlap) { minOverlap = overlapRight;  push = {  overlapRight,  0             }; }
    if (overlapTop    < minOverlap) { minOverlap = overlapTop;    push = {  0,            -overlapTop    }; }
    if (overlapBottom < minOverlap) {                              push = {  0,             overlapBottom }; }

    return Vector2Add(pos, push);
}

struct Obstacle {
    Vector2 center;
    float   amplitude;
    float   speed;
    float   phase;
    float   radius;
    bool    vertical;
};

static Vector2 ObstaclePos(const Obstacle& o, float t) {
    float offset = o.amplitude * sinf(t * o.speed + o.phase);
    return o.vertical
        ? (Vector2){ o.center.x, o.center.y + offset }
        : (Vector2){ o.center.x + offset, o.center.y };
}

static bool CircleHitsAABB(Vector2 circlePos, float circleRadius, Vector2 boxCenter, float boxHalf) {
    float nearestX = Clamp(circlePos.x, boxCenter.x - boxHalf, boxCenter.x + boxHalf);
    float nearestY = Clamp(circlePos.y, boxCenter.y - boxHalf, boxCenter.y + boxHalf);
    float dx = circlePos.x - nearestX;
    float dy = circlePos.y - nearestY;
    return (dx*dx + dy*dy) < (circleRadius * circleRadius);
}

struct Coin {
    Vector2 pos;
    float   radius;
    bool    collected;
};

struct Level {
    std::vector<Rectangle> walls;
    std::vector<Obstacle>  obstacles;
    std::vector<Coin>      coins;
    Rectangle startZone;
    Rectangle endZone;
};

static Level LoadLevel(const char* path) {
    Level lvl = {};

    FILE* f = fopen(path, "r");
    if (!f) {
        TraceLog(LOG_ERROR, "Failed to open level file: %s", path);
        return lvl;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '#') continue;

        char cmd[32];
        if (sscanf(p, "%31s", cmd) != 1) continue;
        const char* args = p + strlen(cmd);

        if (strcmp(cmd, "wall") == 0) {
            float x, y, w, h;
            if (sscanf(args, "%f %f %f %f", &x, &y, &w, &h) == 4)
                lvl.walls.push_back({x, y, w, h});
        } else if (strcmp(cmd, "start") == 0) {
            float x, y, w, h;
            if (sscanf(args, "%f %f %f %f", &x, &y, &w, &h) == 4)
                lvl.startZone = {x, y, w, h};
        } else if (strcmp(cmd, "end") == 0) {
            float x, y, w, h;
            if (sscanf(args, "%f %f %f %f", &x, &y, &w, &h) == 4)
                lvl.endZone = {x, y, w, h};
        } else if (strcmp(cmd, "obstacle") == 0) {
            float cx, cy, amp, speed, phase, radius;
            int vert;
            if (sscanf(args, "%f %f %f %f %f %f %d", &cx, &cy, &amp, &speed, &phase, &radius, &vert) == 7)
                lvl.obstacles.push_back({{cx, cy}, amp, speed, phase, radius, vert != 0});
        } else if (strcmp(cmd, "coin") == 0) {
            float x, y, radius;
            if (sscanf(args, "%f %f %f", &x, &y, &radius) == 3)
                lvl.coins.push_back({{x, y}, radius, false});
        }
    }

    fclose(f);
    return lvl;
}

int main(void) {
    SetConfigFlags(FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT);
    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, WINDOW_TITLE);
    InitAudioDevice();

    int currentLevel = 1;
    const int MAX_LEVEL = 10;

    Level level = LoadLevel(ASSETS_PATH "levels/level1.txt");
    int totalCoins = (int)level.coins.size();

    Vector2 spawn = {
        level.startZone.x + level.startZone.width  / 2.0f,
        level.startZone.y + level.startZone.height / 2.0f
    };
    Vector2 playerPos = spawn;

    int   deaths    = 0;
    int   goldCount = 0;
    float elapsed   = 0.0f;
    bool  debug     = false;
    bool  won       = false;

    bool  hasPowerup    = false;
    bool  slowmoActive  = false;
    float slowmoTimer   = 0.0f;
    const float SLOWMO_DURATION = 3.0f;
    const float SLOWMO_FACTOR   = 0.25f;
    bool    powerupInWorld = false;
    Vector2 powerupWorldPos = {0, 0};
    const float POWERUP_RADIUS = 20.0f;

    auto loadCurrentLevel = [&]() {
        level = LoadLevel(TextFormat(ASSETS_PATH "levels/level%d.txt", currentLevel));
        totalCoins = (int)level.coins.size();
        spawn = {
            level.startZone.x + level.startZone.width  / 2.0f,
            level.startZone.y + level.startZone.height / 2.0f
        };
        playerPos = spawn;
        goldCount = 0;
        elapsed = 0.0f;

        powerupInWorld = false;
        if (currentLevel == 1 || (!hasPowerup && (GetRandomValue(0, 99) < 40))) {
            for (int attempt = 0; attempt < 50; attempt++) {
                float px = (float)GetRandomValue(100, 900);
                float py = (float)GetRandomValue(100, 900);
                bool blocked = false;
                for (const Rectangle& w : level.walls) {
                    if (CheckCollisionCircleRec({px, py}, POWERUP_RADIUS, w)) { blocked = true; break; }
                }
                if (!blocked
                    && !CheckCollisionCircleRec({px, py}, POWERUP_RADIUS, level.startZone)
                    && !CheckCollisionCircleRec({px, py}, POWERUP_RADIUS, level.endZone)) {
                    powerupWorldPos = {px, py};
                    powerupInWorld = true;
                    break;
                }
            }
        }
    };

    auto resetRun = [&]() {
        playerPos = spawn;
        for (Coin& coin : level.coins) coin.collected = false;
        goldCount = 0;
        slowmoActive = false;
        slowmoTimer = 0.0f;
    };

    Texture2D playerTex      = LoadTexture(ASSETS_PATH "player.png");
    Texture2D collectableTex  = LoadTexture(ASSETS_PATH "collectable.png");
    Texture2D powerupTex      = LoadTexture(ASSETS_PATH "powerup.png");

    Music music      = LoadMusicStream(ASSETS_PATH "audio/2s.wav");
    Music slowMusic  = LoadMusicStream(ASSETS_PATH "audio/slow.wav");
    Sound coinPickupSound = LoadSound(ASSETS_PATH "audio/coin.wav");
    Sound deathSound = LoadSound(ASSETS_PATH "audio/death.wav");

    const float MUSIC_VOL = 0.67f;
    const float CROSSFADE_SPEED = 4.0f;
    float musicBlend = 0.0f;

    PlayMusicStream(music);
    PlayMusicStream(slowMusic);
    SetMusicVolume(music, MUSIC_VOL);
    SetMusicVolume(slowMusic, 0.0f);

    RenderTexture2D sceneTex = LoadRenderTexture(SCREEN_WIDTH, SCREEN_HEIGHT);
    Shader bloomShader = LoadShaderFromMemory(NULL, bloomFragShaderSrc);
    int bloomTexelSizeLoc = GetShaderLocation(bloomShader, "texelSize");
    int bloomThresholdLoc = GetShaderLocation(bloomShader, "threshold");
    int bloomIntensityLoc = GetShaderLocation(bloomShader, "intensity");
    int bloomSamplesLoc   = GetShaderLocation(bloomShader, "bloomSamples");
    int bloomStrengthLoc  = GetShaderLocation(bloomShader, "bloomStrength");
    Vector2 bloomTexelSize = { 1.0f / SCREEN_WIDTH, 1.0f / SCREEN_HEIGHT };
    float bloomThreshold = 0.35f;
    float bloomIntensity = 2.5f;
    int   bloomSamples   = BLOOM_SAMPLES;
    float bloomStrength  = BLOOM_STRENGTH;
    SetShaderValue(bloomShader, bloomTexelSizeLoc, &bloomTexelSize, SHADER_UNIFORM_VEC2);
    SetShaderValue(bloomShader, bloomThresholdLoc, &bloomThreshold, SHADER_UNIFORM_FLOAT);
    SetShaderValue(bloomShader, bloomIntensityLoc, &bloomIntensity, SHADER_UNIFORM_FLOAT);
    SetShaderValue(bloomShader, bloomSamplesLoc,   &bloomSamples,   SHADER_UNIFORM_INT);
    SetShaderValue(bloomShader, bloomStrengthLoc,  &bloomStrength,  SHADER_UNIFORM_FLOAT);

    while (!WindowShouldClose()) {
        float delta = GetFrameTime();

        if (slowmoActive) {
            slowmoTimer -= delta;
            if (slowmoTimer <= 0.0f) { slowmoActive = false; slowmoTimer = 0.0f; }
        }
        float gameDelta = slowmoActive ? delta * SLOWMO_FACTOR : delta;
        elapsed += gameDelta;

        float blendTarget = slowmoActive ? 1.0f : 0.0f;
        if (musicBlend < blendTarget) musicBlend = fminf(musicBlend + delta * CROSSFADE_SPEED, blendTarget);
        else if (musicBlend > blendTarget) musicBlend = fmaxf(musicBlend - delta * CROSSFADE_SPEED, blendTarget);
        SetMusicVolume(music, MUSIC_VOL * (1.0f - musicBlend));
        SetMusicVolume(slowMusic, MUSIC_VOL * musicBlend);

        UpdateMusicStream(music);
        UpdateMusicStream(slowMusic);

        if (IsKeyPressed(KEY_L)) debug = !debug;
        if (IsKeyPressed(KEY_R)) { deaths++; resetRun(); }

        if (debug) {
            for (int n = 1; n <= MAX_LEVEL; n++) {
                if (IsKeyPressed(KEY_ZERO + n)) { currentLevel = n; loadCurrentLevel(); won = false; }
            }
            if (IsKeyPressed(KEY_ZERO)) { currentLevel = 10; loadCurrentLevel(); won = false; }
            if (IsKeyPressed(KEY_P)) won = !won;
        }

        Vector2 move = { 0, 0 };
        if (IsKeyPressed(KEY_E) && (hasPowerup || debug)) {
            hasPowerup = false;
            slowmoActive = true;
            slowmoTimer = SLOWMO_DURATION;
        }

        gamepad = -1;
        for (int i = 0; i < 4; i++) { if (IsGamepadAvailable(i)) { gamepad = i; break; } }

        if (!won) {
            if (gamepad >= 0) {
                if (IsGamepadButtonPressed(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN)) {
                    if (hasPowerup || debug) {
                        hasPowerup = false;
                        slowmoActive = true;
                        slowmoTimer = SLOWMO_DURATION;
                    }
                }

                if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) move.x += 1;
                if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT)) move.x -= 1;
                if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN))  move.y += 1;
                if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_UP))    move.y -= 1;

                const float leftStickDeadzoneX = 0.1f;
                const float leftStickDeadzoneY = 0.1f;

                float axisX = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_X);
                float axisY = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_Y);

                if (axisX > -leftStickDeadzoneX && axisX < leftStickDeadzoneX) axisX = 0.0f;
                if (axisY > -leftStickDeadzoneY && axisY < leftStickDeadzoneY) axisY = 0.0f;

                move.x += axisX;
                move.y += axisY;
            }
            
            if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) move.x += 1;
            if (IsKeyDown(KEY_LEFT)  || IsKeyDown(KEY_A)) move.x -= 1;
            if (IsKeyDown(KEY_DOWN)  || IsKeyDown(KEY_S)) move.y += 1;
            if (IsKeyDown(KEY_UP)    || IsKeyDown(KEY_W)) move.y -= 1;
        }

        float len = sqrtf(move.x * move.x + move.y * move.y);
        if (len > 0.0f) { move.x /= len; move.y /= len; }

        playerPos.x += move.x * PLAYER_SPEED * gameDelta;
        playerPos.y += move.y * PLAYER_SPEED * gameDelta;

        const float half = PLAYER_SIZE / 2.0f;
        for (int pass = 0; pass < 3; ++pass)
            for (const Rectangle& wall : level.walls)
                playerPos = ResolveWall(playerPos, half, wall);

        for (int i = 0; i < (int)level.obstacles.size(); i++) {
            Vector2 obsPos = ObstaclePos(level.obstacles[i], elapsed);
            if (CircleHitsAABB(obsPos, level.obstacles[i].radius, playerPos, half)) {
                deaths++;
                PlaySound(deathSound);
                resetRun();
                break;
            }
        }

        for (Coin& coin : level.coins) {
            if (!coin.collected) {
                float dx = playerPos.x - coin.pos.x;
                float dy = playerPos.y - coin.pos.y;
                if ((dx*dx + dy*dy) < (half + coin.radius) * (half + coin.radius)) {
                    coin.collected = true;
                    goldCount++;
                    PlaySound(coinPickupSound);
                }
            }
        }

        if (powerupInWorld && !hasPowerup) {
            float dx = playerPos.x - powerupWorldPos.x;
            float dy = playerPos.y - powerupWorldPos.y;
            if ((dx*dx + dy*dy) < (half + POWERUP_RADIUS) * (half + POWERUP_RADIUS)) {
                hasPowerup = true;
                powerupInWorld = false;
            }
        }

        Rectangle playerRect = { playerPos.x - half, playerPos.y - half, PLAYER_SIZE, PLAYER_SIZE };
        if (!won && goldCount == totalCoins && CheckCollisionRecs(playerRect, level.endZone)) {
            if (currentLevel < MAX_LEVEL) {
                currentLevel++;
                loadCurrentLevel();
            } else {
                won = true;
            }
        }

        BeginTextureMode(sceneTex);
        ClearBackground({32, 32, 32, 255});

        DrawRectangleRec(level.startZone, {  4, 4,  4, 255 });
        DrawRectangleRec(level.endZone, goldCount == totalCoins ? Color{0, 180, 0, 255} : Color{80, 80, 80, 255});

        for (const Rectangle& wall : level.walls)
            DrawRectangleRec(wall, {128, 128, 128, 255});

        for (const Coin& coin : level.coins) {
            if (!coin.collected) {
                float d = coin.radius * 2.0f;
                Rectangle src = { 0, 0, (float)collectableTex.width, (float)collectableTex.height };
                Rectangle dst = { coin.pos.x - coin.radius, coin.pos.y - coin.radius, d, d };
                DrawTexturePro(collectableTex, src, dst, {0, 0}, 0.0f, WHITE);
            }
        }

        if (powerupInWorld) {
            float d = POWERUP_RADIUS * 2.0f;
            Rectangle src = { 0, 0, (float)powerupTex.width, (float)powerupTex.height };
            Rectangle dst = { powerupWorldPos.x - POWERUP_RADIUS, powerupWorldPos.y - POWERUP_RADIUS, d, d };
            DrawTexturePro(powerupTex, src, dst, {0, 0}, 0.0f, WHITE);
        }

        for (int i = 0; i < (int)level.obstacles.size(); i++) {
            Vector2 obsPos = ObstaclePos(level.obstacles[i], elapsed);
            DrawCircleV(obsPos, level.obstacles[i].radius, {220, 120, 60, 255});
        }

        {
            Texture2D tex = slowmoActive ? powerupTex : playerTex;
            Rectangle src = { 0, 0, (float)tex.width, (float)tex.height };
            Rectangle dst = { playerPos.x - half, playerPos.y - half, PLAYER_SIZE, PLAYER_SIZE };
            DrawTexturePro(tex, src, dst, {0, 0}, 0.0f, WHITE);
        }

        DrawText(TextFormat("Level: %d / %d", currentLevel, MAX_LEVEL),  10, 10, 20, LIGHTGRAY);
        DrawText(TextFormat("Deaths: %d", deaths),                         10, 34, 20, LIGHTGRAY);
        {
            const char *coinText = TextFormat("%d / %d", goldCount, totalCoins);
            int textW = MeasureText(coinText, 20);
            float iconSize = 20.0f;
            float gap = 6.0f;
            float totalW = iconSize + gap + textW;
            float startX = SCREEN_WIDTH - 10 - totalW;
            Rectangle csrc = { 0, 0, (float)collectableTex.width, (float)collectableTex.height };
            Rectangle cdst = { startX, 10, iconSize, iconSize };
            DrawTexturePro(collectableTex, csrc, cdst, {0, 0}, 0.0f, WHITE);
            DrawText(coinText, (int)(startX + iconSize + gap), 10, 20, {220, 190, 40, 255});
        }

        if (hasPowerup) {
            float iconSz = 28.0f;
            Rectangle psrc = { 0, 0, (float)powerupTex.width, (float)powerupTex.height };
            Rectangle pdst = { SCREEN_WIDTH - 44, 38, iconSz, iconSz };
            DrawTexturePro(powerupTex, psrc, pdst, {0, 0}, 0.0f, WHITE);
            DrawText("(E)", SCREEN_WIDTH - 44, 68, 16, {200, 200, 200, 255});
        }

        if (slowmoActive) {
            DrawRectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, {100, 160, 255, 30});
            float barW = (slowmoTimer / SLOWMO_DURATION) * 200.0f;
            DrawRectangle(SCREEN_WIDTH / 2 - 100, SCREEN_HEIGHT - 50, (int)barW, 10, {100, 160, 255, 200});
            DrawRectangleLines(SCREEN_WIDTH / 2 - 100, SCREEN_HEIGHT - 50, 200, 10, {100, 160, 255, 120});
        }

        if (debug) {
            DrawFPS(10, 36);
            DrawText(TextFormat("pos: %.0f, %.0f", playerPos.x, playerPos.y), 10, 60, 18, LIGHTGRAY);
        }

        DrawText("WASD / arrows to move  |  E powerup  |  R reset  |  L debug", 10, SCREEN_HEIGHT - 28, 18, {100, 100, 100, 255});

        if (won) {
            DrawRectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, {0, 0, 0, 180});
            const char *winText = "YOU WIN!";
            int winFontSize = 100;
            int winW = MeasureText(winText, winFontSize);
            DrawText(winText, (SCREEN_WIDTH - winW) / 2, (SCREEN_HEIGHT - winFontSize) / 2, winFontSize, LIGHTGRAY);
        }

        EndTextureMode();
        BeginDrawing();
        ClearBackground(BLACK);
        BeginShaderMode(bloomShader);
        DrawTextureRec(sceneTex.texture,
            { 0.0f, 0.0f, (float)SCREEN_WIDTH, -(float)SCREEN_HEIGHT },
            { 0.0f, 0.0f }, WHITE);
        EndShaderMode();
        EndDrawing();
    }

    UnloadRenderTexture(sceneTex);
    UnloadShader(bloomShader);
    UnloadTexture(playerTex);
    UnloadTexture(collectableTex);
    UnloadTexture(powerupTex);
    UnloadMusicStream(music);
    UnloadMusicStream(slowMusic);
    UnloadSound(coinPickupSound);
    UnloadSound(deathSound);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}

