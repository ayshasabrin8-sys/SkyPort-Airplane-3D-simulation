#include <windows.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <cmath>
#include <cstdio>

// Camera controls
float camAngleY = 42.0f;
float camAngleX = 18.0f;
float camDist   = 28.0f;

// Unified Aircraft State Machine (Supports continuous Boarding -> Takeoff -> Landing -> Deboarding loop)
enum FlightState {
    PARKED_AT_GATE,
    BOARDING,
    TAXI_TO_RUNWAY,
    TAKEOFF_ROLL,
    CLIMB,
    IN_AIR,
    APPROACH,
    TOUCHDOWN,
    TAXI_TO_GATE,
    DEBOARDING
};

struct Aircraft {
    const char* code;
    FlightState state;
    float x, y, z;
    float yaw, pitch, speed;
    float gateX, gateZ;         // Assigned parking gate coordinates
    float stripeR, stripeG, stripeB;
    int passengersProcessed;    // Tracks boarding/deboarding progress (0 to 4)
};

// Two Independent Flights (Flight 1 starts at Gate 1, Flight 2 starts in sky or Bay 2)
Aircraft flights[2] = {
    { "BG-101", PARKED_AT_GATE, -5.2f, 0.85f, -13.5f, 90.0f, 0.0f, 0.0f, -5.2f, -13.5f, 0.12f, 0.32f, 0.68f, 0 },
    { "EK-582", IN_AIR,          0.0f, 18.0f, -34.0f,  0.0f, 0.0f, 0.0f, -5.2f,   8.5f, 0.82f, 0.16f, 0.20f, 0 }
};

int selectedFlight = 0; // 0 = BG-101, 1 = EK-582 (Switch with keys '1' and '2')

// Active Boarding / Deboarding Passenger Animation
struct GatePassenger {
    bool active;
    float x, y, z;
    float walkPhase;
    float r, g, b;
    int targetFlight;
    bool isBoarding; // true = Terminal -> Plane, false = Plane -> Terminal
};

GatePassenger gatePax[4];
float paxSpawnTimer = 0.0f;

// Ambient Walking Passengers on Terminal Sidewalk
struct SidewalkPassenger {
    float x, z, speed, walkPhase;
    float r, g, b;
    int dir;
};

SidewalkPassenger sidewalkPeople[4] = {
    {-7.55f, -15.5f, 0.016f, 0.0f, 0.85f, 0.22f, 0.25f,  1},
    {-7.15f, -10.5f, 0.014f, 1.5f, 0.18f, 0.48f, 0.85f, -1},
    {-7.65f,  -4.5f, 0.017f, 3.0f, 0.92f, 0.62f, 0.12f,  1},
    {-7.20f,   2.5f, 0.015f, 4.2f, 0.55f, 0.22f, 0.72f, -1}
};

// Ground Service Truck
float truckZ = -18.0f;
int truckDir = 1;

// Environment animation variables
float cloudOffset = 0.0f;
float radarAngle  = 0.0f;
float arrowBob    = 0.0f;
bool isNight      = false;
bool isPaused     = false;
HWND g_hwnd       = NULL;

// Helper: Emissive Glow
void setGlow(float r, float g, float b) {
    GLfloat emit[] = { r, g, b, 1.0f };
    glMaterialfv(GL_FRONT, GL_EMISSION, emit);
}
void clearGlow() {
    GLfloat noEmit[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    glMaterialfv(GL_FRONT, GL_EMISSION, noEmit);
}

// Helper: Solid 3D Box
void drawBox(float sx, float sy, float sz) {
    float x = sx * 0.5f, y = sy * 0.5f, z = sz * 0.5f;
    glBegin(GL_QUADS);
    glNormal3f( 0,  0,  1); glVertex3f(-x,-y, z); glVertex3f( x,-y, z); glVertex3f( x, y, z); glVertex3f(-x, y, z);
    glNormal3f( 0,  0, -1); glVertex3f( x,-y,-z); glVertex3f(-x,-y,-z); glVertex3f(-x, y,-z); glVertex3f( x, y,-z);
    glNormal3f(-1,  0,  0); glVertex3f(-x,-y,-z); glVertex3f(-x,-y, z); glVertex3f(-x, y, z); glVertex3f(-x, y,-z);
    glNormal3f( 1,  0,  0); glVertex3f( x,-y, z); glVertex3f( x,-y,-z); glVertex3f( x, y,-z); glVertex3f( x, y, z);
    glNormal3f( 0,  1,  0); glVertex3f(-x, y, z); glVertex3f( x, y, z); glVertex3f( x, y,-z); glVertex3f(-x, y,-z);
    glNormal3f( 0, -1,  0); glVertex3f(-x,-y,-z); glVertex3f( x,-y,-z); glVertex3f( x,-y, z); glVertex3f(-x,-y, z);
    glEnd();
}

// Helper: Smooth Cylinder/Cone along +Z
void drawCylinder(float baseR, float topR, float length, int slices = 24) {
    GLUquadric* q = gluNewQuadric();
    gluCylinder(q, baseR, topR, length, slices, 1);
    glPushMatrix(); glRotatef(180.0f, 1.0f, 0.0f, 0.0f); gluDisk(q, 0.0f, baseR, slices, 1); glPopMatrix();
    glPushMatrix(); glTranslatef(0.0f, 0.0f, length); gluDisk(q, 0.0f, topR, slices, 1); glPopMatrix();
    gluDeleteQuadric(q);
}

// Helper: Smooth Sphere
void drawSphere(float radius, int slices = 16, int stacks = 12) {
    GLUquadric* q = gluNewQuadric();
    gluSphere(q, radius, slices, stacks);
    gluDeleteQuadric(q);
}

// Helper: Swept Wing with Winglet
void drawSweptWing(float span, float rootChord, float tipChord, float sweepBack, float thickness, float side, bool addWinglet = false) {
    float x0 = 0.0f, x1 = side * span;
    float yTop = thickness * 0.5f, yBot = -thickness * 0.5f;
    float zRootFront = rootChord * 0.5f, zRootBack = -rootChord * 0.5f;
    float zTipFront  = zRootFront - sweepBack, zTipBack = zTipFront - tipChord;

    glBegin(GL_QUADS);
    glNormal3f(0.0f, 1.0f, 0.0f);
    glVertex3f(x0, yTop, zRootFront); glVertex3f(x1, yTop, zTipFront);
    glVertex3f(x1, yTop, zTipBack);   glVertex3f(x0, yTop, zRootBack);
    glNormal3f(0.0f, -1.0f, 0.0f);
    glVertex3f(x0, yBot, zRootBack);  glVertex3f(x1, yBot, zTipBack);
    glVertex3f(x1, yBot, zTipFront);  glVertex3f(x0, yBot, zRootFront);
    glNormal3f(0.0f, 0.2f, 1.0f);
    glVertex3f(x0, yBot, zRootFront); glVertex3f(x1, yBot, zTipFront);
    glVertex3f(x1, yTop, zTipFront);  glVertex3f(x0, yTop, zRootFront);
    glNormal3f(0.0f, 0.0f, -1.0f);
    glVertex3f(x0, yTop, zRootBack);  glVertex3f(x1, yTop, zTipBack);
    glVertex3f(x1, yBot, zTipBack);   glVertex3f(x0, yBot, zRootBack);
    glNormal3f(side, 0.0f, 0.0f);
    glVertex3f(x1, yBot, zTipFront);  glVertex3f(x1, yBot, zTipBack);
    glVertex3f(x1, yTop, zTipBack);   glVertex3f(x1, yTop, zTipFront);
    glEnd();

    if (addWinglet) {
        glPushMatrix();
        glTranslatef(x1, 0.0f, (zTipFront + zTipBack) * 0.5f);
        glRotatef(-side * 18.0f, 0.0f, 0.0f, 1.0f);
        glTranslatef(0.0f, 0.26f, -0.05f);
        drawBox(0.04f, 0.52f, tipChord * 0.85f);
        glPopMatrix();
    }
}

// Realistic 3-Tiered Evergreen Conifer Tree
void drawRealisticTree(float x, float z, float scale) {
    glPushMatrix();
    glTranslatef(x, 0.0f, z);
    glScalef(scale, scale, scale);

    glColor3f(isNight ? 0.02f : 0.12f, isNight ? 0.08f : 0.32f, isNight ? 0.03f : 0.12f);
    glPushMatrix(); glTranslatef(0.0f, 0.02f, 0.0f); glRotatef(-90.0f, 1.0f, 0.0f, 0.0f);
    GLUquadric* q = gluNewQuadric(); gluDisk(q, 0.0f, 1.15f, 16, 1); gluDeleteQuadric(q);
    glPopMatrix();

    glColor3f(0.36f, 0.22f, 0.11f);
    glPushMatrix(); glRotatef(-90.0f, 1.0f, 0.0f, 0.0f); drawCylinder(0.20f, 0.13f, 1.1f, 12); glPopMatrix();

    float tierY[] = { 0.95f, 1.65f, 2.25f };
    float tierR[] = { 1.15f, 0.88f, 0.58f };
    float tierH[] = { 1.15f, 1.00f, 0.85f };
    float green[] = { 0.32f, 0.40f, 0.48f };

    for (int i = 0; i < 3; i++) {
        glColor3f(isNight ? 0.04f : 0.10f, isNight ? (0.14f + i * 0.03f) : green[i], isNight ? 0.06f : 0.14f);
        glPushMatrix(); glTranslatef(0.0f, tierY[i], 0.0f); glRotatef(-90.0f, 1.0f, 0.0f, 0.0f);
        drawCylinder(tierR[i], 0.02f, tierH[i], 16); glPopMatrix();
    }
    glPopMatrix();
}

// Apron Street Lamp
void drawStreetLamp(float x, float z) {
    glPushMatrix();
    glTranslatef(x, 0.0f, z);
    glColor3f(0.35f, 0.38f, 0.42f);
    glPushMatrix(); glRotatef(-90.0f, 1.0f, 0.0f, 0.0f); drawCylinder(0.07f, 0.05f, 3.6f, 10); glPopMatrix();
    glPushMatrix(); glTranslatef(0.35f, 3.55f, 0.0f); drawBox(0.8f, 0.06f, 0.06f); glPopMatrix();

    if (isNight) { setGlow(1.0f, 0.88f, 0.55f); glColor3f(1.0f, 0.92f, 0.65f); }
    else         { glColor3f(0.85f, 0.85f, 0.88f); }
    glPushMatrix(); glTranslatef(0.65f, 3.48f, 0.0f); drawSphere(0.16f, 10, 10); glPopMatrix();
    clearGlow();
    glPopMatrix();
}

// 1. Airport Environment
void drawEnvironment() {
    glColor3f(isNight ? 0.05f : 0.24f, isNight ? 0.16f : 0.50f, isNight ? 0.08f : 0.21f);
    glPushMatrix(); glTranslatef(0.0f, -0.05f, 0.0f); drawBox(120.0f, 0.1f, 120.0f); glPopMatrix();

    // Distant Hills
    glColor3f(isNight ? 0.04f : 0.18f, isNight ? 0.12f : 0.40f, isNight ? 0.10f : 0.24f);
    float hillX[] = {-38.0f, -15.0f, 14.0f, 38.0f};
    float hillR[] = {15.0f, 19.0f, 17.0f, 16.0f};
    for (int i = 0; i < 4; i++) {
        glPushMatrix(); glTranslatef(hillX[i], -2.0f, 48.0f); glScalef(1.5f, 0.45f, 1.0f); drawSphere(hillR[i], 18, 12); glPopMatrix();
    }

    // Main Runway
    glColor3f(isNight ? 0.12f : 0.17f, isNight ? 0.13f : 0.18f, isNight ? 0.16f : 0.20f);
    glPushMatrix(); glTranslatef(0.0f, 0.01f, 0.0f); drawBox(6.4f, 0.05f, 52.0f); glPopMatrix();

    glColor3f(0.92f, 0.92f, 0.92f);
    glPushMatrix(); glTranslatef(-3.05f, 0.038f, 0.0f); drawBox(0.15f, 0.01f, 51.0f); glPopMatrix();
    glPushMatrix(); glTranslatef( 3.05f, 0.038f, 0.0f); drawBox(0.15f, 0.01f, 51.0f); glPopMatrix();

    for (float x = -2.4f; x <= 2.4f; x += 0.8f) {
        glPushMatrix(); glTranslatef(x, 0.04f, -23.5f); drawBox(0.4f, 0.01f, 2.5f); glPopMatrix();
        glPushMatrix(); glTranslatef(x, 0.04f,  23.5f); drawBox(0.4f, 0.01f, 2.5f); glPopMatrix();
    }
    for (float z = -19.0f; z <= 19.0f; z += 4.0f) {
        glPushMatrix(); glTranslatef(0.0f, 0.04f, z); drawBox(0.28f, 0.01f, 2.0f); glPopMatrix();
    }

    // Concrete Apron & Dual Taxiways
    glColor3f(isNight ? 0.20f : 0.34f, isNight ? 0.21f : 0.35f, isNight ? 0.24f : 0.37f);
    glPushMatrix(); glTranslatef(-5.0f, 0.008f, -2.0f); drawBox(6.8f, 0.04f, 34.0f); glPopMatrix();

    // Yellow Taxiway Guidelines (Gate 1 at z=-13.5, Bay 2 at z=8.5)
    glColor3f(0.95f, 0.78f, 0.10f);
    glPushMatrix(); glTranslatef(-4.2f, 0.035f, -13.5f); drawBox(5.2f, 0.01f, 0.18f); glPopMatrix();
    glPushMatrix(); glTranslatef(-4.2f, 0.035f,   8.5f); drawBox(5.2f, 0.01f, 0.18f); glPopMatrix();

    // Passenger Sidewalk
    glColor3f(0.60f, 0.61f, 0.64f);
    glPushMatrix(); glTranslatef(-7.4f, 0.04f, -2.5f); drawBox(1.8f, 0.08f, 28.0f); glPopMatrix();
    glColor3f(0.95f, 0.78f, 0.10f);
    glPushMatrix(); glTranslatef(-6.52f, 0.082f, -2.5f); drawBox(0.10f, 0.01f, 28.0f); glPopMatrix();

    for (float z = -14.0f; z <= 9.0f; z += 7.5f) drawStreetLamp(-6.7f, z);

    // Runway Edge Lights
    for (float z = -24.0f; z <= 24.0f; z += 3.5f) {
        if (isNight) setGlow(1.0f, 0.80f, 0.15f);
        glColor3f(isNight ? 1.0f : 0.85f, isNight ? 0.85f : 0.70f, 0.15f);
        glPushMatrix(); glTranslatef(-3.25f, 0.12f, z); drawSphere(0.13f, 8, 8); glPopMatrix();
        glPushMatrix(); glTranslatef( 3.25f, 0.12f, z); drawSphere(0.13f, 8, 8); glPopMatrix();
        clearGlow();
    }

    // Sun / Moon & Stars
    glPushMatrix();
    glTranslatef(24.0f, 27.0f, 38.0f);
    if (isNight) { setGlow(0.85f, 0.90f, 1.0f); glColor3f(0.95f, 0.97f, 1.0f); }
    else         { setGlow(0.4f, 0.35f, 0.1f);  glColor3f(1.0f, 0.90f, 0.35f); }
    drawSphere(2.2f, 18, 18);
    clearGlow();
    glPopMatrix();

    if (isNight) {
        setGlow(1.0f, 1.0f, 1.0f); glColor3f(1.0f, 1.0f, 1.0f);
        float stars[][3] = {{-30,32,35},{-12,36,40},{5,34,42},{18,38,30},{-22,28,-30},{12,33,-35},{32,30,-20},{-35,35,10}};
        for (int i = 0; i < 8; i++) {
            glPushMatrix(); glTranslatef(stars[i][0], stars[i][1], stars[i][2]); drawSphere(0.18f, 6, 6); glPopMatrix();
        }
        clearGlow();
    }

    // Realistic Trees
    drawRealisticTree(-15.5f, -20.0f, 1.15f);
    drawRealisticTree(-16.2f, -12.0f, 0.95f);
    drawRealisticTree(-15.4f,   3.5f, 1.20f);
    drawRealisticTree(-15.8f,  15.5f, 1.05f);
    drawRealisticTree(  8.5f, -18.5f, 1.10f);
    drawRealisticTree(  9.2f,  -2.5f, 1.25f);
    drawRealisticTree(  8.8f,  12.0f, 1.00f);
}

// 2. Drifting 3D Clouds
void drawSingleCloud(float x, float y, float z, float scale) {
    glPushMatrix();
    glTranslatef(x, y, z);
    glScalef(scale, scale * 0.65f, scale);
    glColor3f(isNight ? 0.18f : 0.98f, isNight ? 0.22f : 0.98f, isNight ? 0.32f : 1.0f);
    drawSphere(1.6f, 14, 12);
    glPushMatrix(); glTranslatef( 1.4f, -0.2f,  0.2f); drawSphere(1.2f, 12, 10); glPopMatrix();
    glPushMatrix(); glTranslatef(-1.4f, -0.2f, -0.1f); drawSphere(1.3f, 12, 10); glPopMatrix();
    glPushMatrix(); glTranslatef( 0.5f,  0.5f, -0.5f); drawSphere(1.1f, 12, 10); glPopMatrix();
    glPopMatrix();
}

void drawClouds() {
    float baseClouds[][4] = {{-18,20,15,1.3f},{6,23,26,1.6f},{22,19,5,1.2f},{-25,21,-15,1.4f},{14,20,-22,1.1f}};
    for (int i = 0; i < 5; i++) {
        float cx = baseClouds[i][0] + cloudOffset;
        if (cx > 48.0f) cx -= 96.0f;
        drawSingleCloud(cx, baseClouds[i][1], baseClouds[i][2], baseClouds[i][3]);
    }
}

// 3. Modern Terminal Building, Hangar & ATC Tower
void drawBuildings() {
    // Extended Terminal Building (covers both Gate 1 at z=-13.5 and Gate 2 at z=8.5)
    glColor3f(0.90f, 0.92f, 0.95f);
    glPushMatrix(); glTranslatef(-10.8f, 1.4f, -2.5f); drawBox(4.6f, 2.8f, 25.0f); glPopMatrix();

    // Curved Modern Roof
    glColor3f(0.16f, 0.28f, 0.46f);
    glPushMatrix();
    glTranslatef(-10.4f, 2.8f, -15.2f);
    glScalef(2.7f, 0.55f, 1.0f);
    drawCylinder(1.05f, 1.05f, 25.4f, 24);
    glPopMatrix();

    // Illuminated Glass Curtain Wall
    if (isNight) setGlow(0.85f, 0.72f, 0.30f);
    glColor3f(isNight ? 1.0f : 0.32f, isNight ? 0.88f : 0.70f, isNight ? 0.45f : 0.94f);
    glPushMatrix(); glTranslatef(-8.48f, 1.45f, -2.5f); drawBox(0.08f, 1.85f, 23.5f); glPopMatrix();
    clearGlow();

    // Steel Window Frames
    glColor3f(0.25f, 0.28f, 0.32f);
    for (float z = -14.0f; z <= 9.0f; z += 2.3f) {
        glPushMatrix(); glTranslatef(-8.42f, 1.45f, z); drawBox(0.12f, 2.75f, 0.14f); glPopMatrix();
    }

    // Gate 1 & Gate 2 Terminal Doors
    glColor3f(0.12f, 0.16f, 0.22f);
    glPushMatrix(); glTranslatef(-8.44f, 0.85f, -12.0f); drawBox(0.15f, 1.6f, 1.2f); glPopMatrix();
    glPushMatrix(); glTranslatef(-8.44f, 0.85f,  10.0f); drawBox(0.15f, 1.6f, 1.2f); glPopMatrix();

    // Aircraft Maintenance Hangar
    glColor3f(0.75f, 0.78f, 0.82f);
    glPushMatrix(); glTranslatef(8.5f, 1.4f, -9.0f); drawBox(5.0f, 2.8f, 7.0f); glPopMatrix();
    glColor3f(0.24f, 0.34f, 0.46f);
    glPushMatrix(); glTranslatef(8.5f, 2.8f, -12.6f); glScalef(2.6f, 0.65f, 1.0f); drawCylinder(1.0f, 1.0f, 7.2f, 20); glPopMatrix();
    if (isNight) setGlow(0.55f, 0.50f, 0.35f);
    glColor3f(isNight ? 0.85f : 0.16f, isNight ? 0.78f : 0.17f, isNight ? 0.55f : 0.20f);
    glPushMatrix(); glTranslatef(5.98f, 1.25f, -9.0f); drawBox(0.1f, 2.3f, 5.2f); glPopMatrix();
    clearGlow();

    // Cylindrical ATC Control Tower
    float tx = -10.2f, tz = 13.5f;
    glColor3f(0.80f, 0.82f, 0.86f);
    glPushMatrix(); glTranslatef(tx, 1.0f, tz); drawBox(3.4f, 2.0f, 3.4f); glPopMatrix();
    glColor3f(0.92f, 0.93f, 0.96f);
    glPushMatrix(); glTranslatef(tx, 2.0f, tz); glRotatef(-90.0f, 1.0f, 0.0f, 0.0f); drawCylinder(1.05f, 0.75f, 6.2f, 24); glPopMatrix();
    glColor3f(0.22f, 0.26f, 0.32f);
    glPushMatrix(); glTranslatef(tx, 8.2f, tz); glRotatef(-90.0f, 1.0f, 0.0f, 0.0f); drawCylinder(1.45f, 1.55f, 0.35f, 24); glPopMatrix();

    if (isNight) setGlow(0.85f, 0.75f, 0.35f);
    glColor3f(isNight ? 0.98f : 0.25f, isNight ? 0.85f : 0.68f, isNight ? 0.40f : 0.92f);
    glPushMatrix(); glTranslatef(tx, 8.55f, tz); glRotatef(-90.0f, 1.0f, 0.0f, 0.0f); drawCylinder(1.25f, 1.65f, 1.3f, 24); glPopMatrix();
    clearGlow();

    glColor3f(0.82f, 0.18f, 0.18f);
    glPushMatrix(); glTranslatef(tx, 9.85f, tz); glRotatef(-90.0f, 1.0f, 0.0f, 0.0f); drawCylinder(1.75f, 0.4f, 0.45f, 24); glPopMatrix();

    glPushMatrix();
    glTranslatef(tx, 10.5f, tz);
    glRotatef(radarAngle, 0.0f, 1.0f, 0.0f);
    glColor3f(0.95f, 0.95f, 0.95f); drawBox(1.6f, 0.35f, 0.12f);
    setGlow(1.0f, 0.1f, 0.1f); glColor3f(1.0f, 0.1f, 0.1f); glTranslatef(0.0f, 0.35f, 0.0f); drawSphere(0.15f, 10, 10);
    clearGlow();
    glPopMatrix();
}

// Helper: Draw a single articulated 3D person with rolling luggage
void drawPersonWithLuggage(float x, float y, float z, float yawDeg, float walkPhase, float r, float g, float b) {
    glPushMatrix();
    float bobY = y + fabs(sin(walkPhase)) * 0.025f;
    glTranslatef(x, bobY, z);
    glRotatef(yawDeg, 0.0f, 1.0f, 0.0f);

    float legSwing = sin(walkPhase) * 30.0f;

    // Left & Right Legs
    for (int side = -1; side <= 1; side += 2) {
        glPushMatrix();
        glTranslatef(side * 0.075f, 0.42f, 0.0f);
        glRotatef(side * legSwing, 1.0f, 0.0f, 0.0f);
        glColor3f(0.15f, 0.18f, 0.25f);
        glPushMatrix(); glRotatef(90.0f, 1.0f, 0.0f, 0.0f); drawCylinder(0.048f, 0.038f, 0.38f, 10); glPopMatrix();
        glColor3f(0.12f, 0.12f, 0.14f);
        glTranslatef(0.0f, -0.39f, 0.03f);
        drawBox(0.08f, 0.05f, 0.15f);
        glPopMatrix();
    }

    // Torso
    glColor3f(r, g, b);
    glPushMatrix();
    glTranslatef(0.0f, 0.42f, 0.0f); glRotatef(-90.0f, 1.0f, 0.0f, 0.0f); glScalef(1.15f, 0.72f, 1.0f);
    drawCylinder(0.11f, 0.13f, 0.36f, 14);
    glPopMatrix();

    // Backpack
    glColor3f(0.22f, 0.24f, 0.28f);
    glPushMatrix(); glTranslatef(0.0f, 0.60f, -0.11f); drawBox(0.20f, 0.24f, 0.10f); glPopMatrix();

    // Head & Hair
    glColor3f(0.92f, 0.74f, 0.60f);
    glPushMatrix(); glTranslatef(0.0f, 0.88f, 0.0f); drawSphere(0.105f, 12, 12); glPopMatrix();
    glColor3f(0.14f, 0.10f, 0.08f);
    glPushMatrix(); glTranslatef(0.0f, 0.91f, -0.02f); drawSphere(0.106f, 12, 10); glPopMatrix();

    // Right Arm
    glPushMatrix();
    glTranslatef(0.155f, 0.74f, 0.0f);
    glRotatef(-legSwing * 0.75f, 1.0f, 0.0f, 0.0f);
    glColor3f(r, g, b);
    glPushMatrix(); glRotatef(90.0f, 1.0f, 0.0f, 0.0f); drawCylinder(0.038f, 0.032f, 0.28f, 10); glPopMatrix();
    glColor3f(0.92f, 0.74f, 0.60f); glTranslatef(0.0f, -0.29f, 0.0f); drawSphere(0.036f, 8, 8);
    glPopMatrix();

    // Left Arm holding Luggage Handle
    glPushMatrix();
    glTranslatef(-0.155f, 0.74f, 0.0f);
    glRotatef(-36.0f, 1.0f, 0.0f, 0.0f);
    glColor3f(r, g, b);
    glPushMatrix(); glRotatef(90.0f, 1.0f, 0.0f, 0.0f); drawCylinder(0.038f, 0.032f, 0.28f, 10); glPopMatrix();
    glColor3f(0.92f, 0.74f, 0.60f); glTranslatef(0.0f, -0.29f, 0.0f); drawSphere(0.036f, 8, 8);
    glPopMatrix();

    // Rolling Suitcase
    glPushMatrix();
    glTranslatef(-0.17f, 0.19f, -0.28f);
    glRotatef(22.0f, 1.0f, 0.0f, 0.0f);
    glColor3f(0.18f, 0.35f, 0.55f); drawBox(0.15f, 0.28f, 0.20f);
    glColor3f(0.10f, 0.10f, 0.12f);
    glPushMatrix(); glTranslatef(-0.06f, -0.15f, -0.07f); drawSphere(0.03f, 8, 8); glPopMatrix();
    glPushMatrix(); glTranslatef( 0.06f, -0.15f, -0.07f); drawSphere(0.03f, 8, 8); glPopMatrix();
    glColor3f(0.80f, 0.82f, 0.85f);
    glTranslatef(0.0f, 0.24f, 0.05f); drawBox(0.025f, 0.24f, 0.025f);
    glTranslatef(0.0f, 0.12f, 0.0f); drawBox(0.09f, 0.025f, 0.03f);
    glPopMatrix();

    glPopMatrix();
}

// Draw Mobile Boarding Stairs Truck whenever a plane is Boarding or Deboarding
void drawBoardingStairs(float gateX, float gateZ) {
    glPushMatrix();
    // Position stairs right next to the aircraft forward cabin door
    glTranslatef(gateX - 1.35f, 0.0f, gateZ + 1.5f);

    // Base Chassis
    glColor3f(0.22f, 0.26f, 0.32f);
    glPushMatrix(); glTranslatef(0.0f, 0.15f, 0.0f); drawBox(1.5f, 0.18f, 0.75f); glPopMatrix();

    // Sloped Stair Steps leading from sidewalk (-X) up to plane door (+X)
    glColor3f(0.85f, 0.88f, 0.92f);
    for (int s = 0; s < 6; s++) {
        float sx = -0.55f + s * 0.22f;
        float sy = 0.25f + s * 0.11f;
        glPushMatrix();
        glTranslatef(sx, sy, 0.0f);
        drawBox(0.24f, 0.08f, 0.68f);
        glPopMatrix();
    }
    // Blue Handrails
    glColor3f(0.15f, 0.45f, 0.85f);
    glPushMatrix(); glTranslatef(0.0f, 0.82f,  0.34f); glRotatef(26.0f, 0.0f, 0.0f, 1.0f); drawBox(1.4f, 0.05f, 0.04f); glPopMatrix();
    glPushMatrix(); glTranslatef(0.0f, 0.82f, -0.34f); glRotatef(26.0f, 0.0f, 0.0f, 1.0f); drawBox(1.4f, 0.05f, 0.04f); glPopMatrix();

    glPopMatrix();
}

// Draw All Passengers (Sidewalk walkers + Active Boarding/Deboarding Passengers)
void drawAllPassengers() {
    // 1. Sidewalk pedestrians
    for (int i = 0; i < 4; i++) {
        float yaw = (sidewalkPeople[i].dir == 1) ? 0.0f : 180.0f;
        drawPersonWithLuggage(sidewalkPeople[i].x, 0.08f, sidewalkPeople[i].z,
                              yaw, sidewalkPeople[i].walkPhase,
                              sidewalkPeople[i].r, sidewalkPeople[i].g, sidewalkPeople[i].b);
    }

    // 2. Active Boarding / Deboarding Passengers climbing or descending stairs into/out of plane
    for (int i = 0; i < 4; i++) {
        if (gatePax[i].active) {
            float yaw = gatePax[i].isBoarding ? 90.0f : -90.0f;
            drawPersonWithLuggage(gatePax[i].x, gatePax[i].y, gatePax[i].z,
                                  yaw, gatePax[i].walkPhase,
                                  gatePax[i].r, gatePax[i].g, gatePax[i].b);
        }
    }
}

// Ground Service Truck
void drawGroundVehicle() {
    glPushMatrix();
    glTranslatef(-6.1f, 0.05f, truckZ);
    if (truckDir == -1) glRotatef(180.0f, 0.0f, 1.0f, 0.0f);

    glColor3f(0.95f, 0.75f, 0.12f);
    glPushMatrix(); glTranslatef(0.0f, 0.26f, 0.0f); drawBox(0.75f, 0.35f, 1.5f); glPopMatrix();
    glColor3f(0.90f, 0.92f, 0.95f);
    glPushMatrix(); glTranslatef(0.0f, 0.52f, 0.35f); drawBox(0.70f, 0.30f, 0.65f); glPopMatrix();

    if (isNight) setGlow(1.0f, 0.95f, 0.75f);
    glColor3f(1.0f, 0.95f, 0.80f);
    glPushMatrix(); glTranslatef(-0.24f, 0.28f, 0.76f); drawSphere(0.06f, 8, 8); glPopMatrix();
    glPushMatrix(); glTranslatef( 0.24f, 0.28f, 0.76f); drawSphere(0.06f, 8, 8); glPopMatrix();
    clearGlow();

    glColor3f(0.12f, 0.12f, 0.14f);
    float wz[] = {-0.48f, 0.48f};
    for (int i = 0; i < 2; i++) {
        glPushMatrix(); glTranslatef(-0.38f, 0.12f, wz[i]); drawSphere(0.12f, 8, 8); glPopMatrix();
        glPushMatrix(); glTranslatef( 0.38f, 0.12f, wz[i]); drawSphere(0.12f, 8, 8); glPopMatrix();
    }
    glPopMatrix();
}

// Detailed Commercial Airliner + Selection Indicator Arrow
void drawAirplaneModel(const Aircraft& ac, bool isSelected) {
    glPushMatrix();
    glTranslatef(ac.x, ac.y, ac.z);

    // Floating Green Selection Indicator above currently selected aircraft
    if (isSelected) {
        glPushMatrix();
        glTranslatef(0.0f, 2.35f + sin(arrowBob) * 0.18f, 0.0f);
        glRotatef(90.0f, 1.0f, 0.0f, 0.0f);
        setGlow(0.1f, 0.85f, 0.25f);
        glColor3f(0.15f, 0.95f, 0.30f);
        drawCylinder(0.28f, 0.01f, 0.45f, 12);
        clearGlow();
        glPopMatrix();
    }

    glRotatef(ac.yaw,    0.0f, 1.0f, 0.0f);
    glRotatef(-ac.pitch, 1.0f, 0.0f, 0.0f);

    // Main Fuselage
    glColor3f(0.98f, 0.98f, 1.0f);
    glPushMatrix(); glTranslatef(0.0f, 0.0f, -2.3f); drawCylinder(0.52f, 0.52f, 4.5f, 28); glPopMatrix();

    // Airline Stripe
    glColor3f(ac.stripeR, ac.stripeG, ac.stripeB);
    glPushMatrix(); glTranslatef(0.0f, -0.08f, -2.3f); drawCylinder(0.525f, 0.525f, 4.5f, 28); glPopMatrix();

    // Nose Cone
    glColor3f(0.98f, 0.98f, 1.0f);
    glPushMatrix(); glTranslatef(0.0f, -0.02f, 2.2f); drawCylinder(0.52f, 0.26f, 0.95f, 28); glPopMatrix();
    glColor3f(ac.stripeR, ac.stripeG, ac.stripeB);
    glPushMatrix(); glTranslatef(0.0f, -0.04f, 3.15f); drawCylinder(0.26f, 0.03f, 0.45f, 24); glPopMatrix();

    // Cockpit Windshield
    glColor3f(0.08f, 0.15f, 0.24f);
    glPushMatrix(); glTranslatef(0.0f, 0.24f, 2.45f); glScalef(0.68f, 0.32f, 0.65f); drawSphere(0.55f, 16, 12); glPopMatrix();

    // Cabin Windows
    if (isNight) setGlow(1.0f, 0.88f, 0.45f);
    glColor3f(isNight ? 1.0f : 0.15f, isNight ? 0.90f : 0.25f, isNight ? 0.50f : 0.38f);
    for (float wz = -1.8f; wz <= 1.7f; wz += 0.36f) {
        glPushMatrix(); glTranslatef(0.0f, 0.14f, wz); drawBox(1.06f, 0.10f, 0.14f); glPopMatrix();
    }
    clearGlow();

    // Tail Cone
    glColor3f(0.96f, 0.96f, 0.99f);
    glPushMatrix(); glTranslatef(0.0f, 0.08f, -3.6f); drawCylinder(0.14f, 0.52f, 1.3f, 24); glPopMatrix();

    // Swept Wings + Winglets
    glColor3f(0.82f, 0.85f, 0.89f);
    glPushMatrix();
    glTranslatef(0.0f, -0.08f, 0.3f);
    drawSweptWing(3.6f, 1.45f, 0.45f, 1.15f, 0.10f,  1.0f, true);
    drawSweptWing(3.6f, 1.45f, 0.45f, 1.15f, 0.10f, -1.0f, true);
    glPopMatrix();

    // Wingtip Lights
    setGlow(1.0f, 0.15f, 0.15f);
    glColor3f(1.0f, 0.15f, 0.15f); glPushMatrix(); glTranslatef( 3.6f, -0.06f, -0.85f); drawSphere(0.09f, 8, 8); glPopMatrix();
    setGlow(0.15f, 1.0f, 0.25f);
    glColor3f(0.15f, 1.0f, 0.25f); glPushMatrix(); glTranslatef(-3.6f, -0.06f, -0.85f); drawSphere(0.09f, 8, 8); glPopMatrix();
    clearGlow();

    // Jet Engines
    float engineX[] = {-1.55f, 1.55f};
    for (int i = 0; i < 2; i++) {
        glPushMatrix();
        glTranslatef(engineX[i], -0.34f, 0.05f);
        glColor3f(0.65f, 0.68f, 0.72f); glPushMatrix(); glTranslatef(0.0f, 0.22f, 0.45f); drawBox(0.10f, 0.22f, 0.65f); glPopMatrix();
        glColor3f(0.88f, 0.90f, 0.92f); drawCylinder(0.24f, 0.22f, 1.05f, 20);
        glColor3f(0.12f, 0.13f, 0.15f); glTranslatef(0.0f, 0.0f, 1.02f); drawCylinder(0.21f, 0.21f, 0.08f, 20);
        glColor3f(0.95f, 0.95f, 0.98f); glTranslatef(0.0f, 0.0f, 0.05f); drawCylinder(0.07f, 0.01f, 0.12f, 12);
        glPopMatrix();
    }

    // Horizontal Stabilizers
    glColor3f(0.82f, 0.85f, 0.89f);
    glPushMatrix();
    glTranslatef(0.0f, 0.16f, -3.1f);
    drawSweptWing(1.35f, 0.70f, 0.25f, 0.45f, 0.07f,  1.0f, false);
    drawSweptWing(1.35f, 0.70f, 0.25f, 0.45f, 0.07f, -1.0f, false);
    glPopMatrix();

    // Vertical Tail Fin
    glColor3f(ac.stripeR, ac.stripeG, ac.stripeB);
    glBegin(GL_QUADS);
    glNormal3f(-1, 0, 0);
    glVertex3f(-0.05f, 0.18f, -2.4f); glVertex3f(-0.05f, 0.18f, -3.5f);
    glVertex3f(-0.03f, 1.58f, -3.8f); glVertex3f(-0.03f, 1.58f, -3.2f);
    glNormal3f(1, 0, 0);
    glVertex3f(0.05f, 0.18f, -3.5f);  glVertex3f(0.05f, 0.18f, -2.4f);
    glVertex3f(0.03f, 1.58f, -3.2f);  glVertex3f(0.03f, 1.58f, -3.8f);
    glNormal3f(0, 1, 0);
    glVertex3f(-0.03f, 1.58f, -3.2f); glVertex3f(-0.03f, 1.58f, -3.8f);
    glVertex3f( 0.03f, 1.58f, -3.8f); glVertex3f( 0.03f, 1.58f, -3.2f);
    glEnd();

    // Retractable Landing Gear
    if (ac.y < 2.5f) {
        glColor3f(0.65f, 0.68f, 0.72f);
        glPushMatrix(); glTranslatef( 0.0f, -0.55f,  2.1f); drawBox(0.06f, 0.25f, 0.06f); glPopMatrix();
        glPushMatrix(); glTranslatef(-0.75f, -0.55f, -0.2f); drawBox(0.07f, 0.25f, 0.07f); glPopMatrix();
        glPushMatrix(); glTranslatef( 0.75f, -0.55f, -0.2f); drawBox(0.07f, 0.25f, 0.07f); glPopMatrix();

        glColor3f(0.14f, 0.14f, 0.16f);
        glPushMatrix(); glTranslatef( 0.0f, -0.68f,  2.1f); drawSphere(0.15f, 10, 10); glPopMatrix();
        glPushMatrix(); glTranslatef(-0.75f, -0.68f, -0.2f); drawSphere(0.18f, 10, 10); glPopMatrix();
        glPushMatrix(); glTranslatef( 0.75f, -0.68f, -0.2f); drawSphere(0.18f, 10, 10); glPopMatrix();
    }
    glPopMatrix();
}

const char* getStateLabel(FlightState st) {
    switch (st) {
        case PARKED_AT_GATE: return "PARKED (Ready [T])";
        case BOARDING:       return "BOARDING PAX...";
        case TAXI_TO_RUNWAY: return "TAXI TO RUNWAY";
        case TAKEOFF_ROLL:   return "TAKEOFF ROLL";
        case CLIMB:          return "CLIMBING";
        case IN_AIR:         return "IN AIR (Ready [L])";
        case APPROACH:       return "LANDING APPROACH";
        case TOUCHDOWN:      return "TOUCHDOWN";
        case TAXI_TO_GATE:   return "TAXI TO GATE";
        case DEBOARDING:     return "DEBOARDING PAX...";
    }
    return "";
}

void updateWindowTitle() {
    char title[240];
    sprintf(title, "SkyPort 3D | Selected: [%s] | 1:BG-101(%s) | 2:EK-582(%s)%s | [1/2]Select [T]akeoff [L]and [N]ight",
            flights[selectedFlight].code,
            getStateLabel(flights[0].state),
            getStateLabel(flights[1].state),
            isPaused ? " [PAUSED]" : "");
    SetWindowText(g_hwnd, title);
}

// Helper: Check if runway is currently busy before allowing another plane to enter
bool isRunwayClear(int myIndex) {
    int other = 1 - myIndex;
    FlightState st = flights[other].state;
    if (st == TAXI_TO_RUNWAY || st == TAKEOFF_ROLL || st == APPROACH || st == TOUCHDOWN || st == TAXI_TO_GATE) {
        return false;
    }
    return true;
}

// Update Boarding & Deboarding Passengers climbing/descending aircraft stairs
void updateGatePassengers() {
    for (int f = 0; f < 2; f++) {
        if (flights[f].state == BOARDING || flights[f].state == DEBOARDING) {
            paxSpawnTimer += 0.02f;
            if (paxSpawnTimer >= 0.85f && flights[f].passengersProcessed < 4) {
                int idx = flights[f].passengersProcessed;
                gatePax[idx].active = true;
                gatePax[idx].targetFlight = f;
                gatePax[idx].walkPhase = 0.0f;
                gatePax[idx].r = (idx % 2 == 0) ? 0.85f : 0.20f;
                gatePax[idx].g = (idx == 1) ? 0.65f : 0.30f;
                gatePax[idx].b = (idx >= 2) ? 0.85f : 0.25f;

                if (flights[f].state == BOARDING) {
                    // Start at Terminal Door (-8.3) and walk toward Plane Door (-5.8)
                    gatePax[idx].isBoarding = true;
                    gatePax[idx].x = -8.3f;
                    gatePax[idx].y = 0.08f;
                    gatePax[idx].z = flights[f].gateZ + 1.5f;
                } else {
                    // Start at Plane Door (-5.8, y=0.85) and walk down stairs to Terminal (-8.3)
                    gatePax[idx].isBoarding = false;
                    gatePax[idx].x = -5.8f;
                    gatePax[idx].y = 0.85f;
                    gatePax[idx].z = flights[f].gateZ + 1.5f;
                }
                flights[f].passengersProcessed++;
                paxSpawnTimer = 0.0f;
            }

            // Move active passengers along sidewalk & boarding stairs
            int stillWalking = 0;
            for (int i = 0; i < 4; i++) {
                if (!gatePax[i].active) continue;
                stillWalking++;
                gatePax[i].walkPhase += 0.16f;

                if (gatePax[i].isBoarding) {
                    gatePax[i].x += 0.022f;
                    // Climb stairs as x moves from -7.1 to -5.8
                    if (gatePax[i].x > -7.1f) {
                        gatePax[i].y = 0.08f + ((gatePax[i].x + 7.1f) / 1.3f) * 0.77f;
                    }
                    // Entered plane door!
                    if (gatePax[i].x >= -5.75f) {
                        gatePax[i].active = false;
                    }
                } else {
                    gatePax[i].x -= 0.022f;
                    // Descend stairs as x moves from -5.8 down to -7.1
                    if (gatePax[i].x > -7.1f) {
                        gatePax[i].y = 0.08f + ((gatePax[i].x + 7.1f) / 1.3f) * 0.77f;
                    } else {
                        gatePax[i].y = 0.08f;
                    }
                    // Entered terminal building!
                    if (gatePax[i].x <= -8.3f) {
                        gatePax[i].active = false;
                    }
                }
            }

            // Once all 4 passengers finish boarding or deboarding, transition automatically!
            if (flights[f].passengersProcessed >= 4 && stillWalking == 0) {
                if (flights[f].state == BOARDING) {
                    flights[f].state = TAXI_TO_RUNWAY;
                } else if (flights[f].state == DEBOARDING) {
                    flights[f].state = PARKED_AT_GATE;
                }
            }
        }
    }
}

void updateSimulation() {
    if (isPaused) return;

    cloudOffset += 0.015f;
    radarAngle  += 1.8f;
    arrowBob    += 0.08f;
    if (radarAngle >= 360.0f) radarAngle -= 360.0f;

    // Sidewalk Pedestrians
    for (int i = 0; i < 4; i++) {
        sidewalkPeople[i].z += sidewalkPeople[i].speed * sidewalkPeople[i].dir;
        sidewalkPeople[i].walkPhase += 0.14f;
        if (sidewalkPeople[i].z > 9.5f)   sidewalkPeople[i].dir = -1;
        if (sidewalkPeople[i].z < -15.5f) sidewalkPeople[i].dir = 1;
    }

    // Ground Service Truck
    truckZ += 0.035f * truckDir;
    if (truckZ > 7.5f)   truckDir = -1;
    if (truckZ < -16.5f) truckDir = 1;

    // Update Boarding/Deboarding Passengers
    updateGatePassengers();

    // Update Both Aircraft State Machines
    for (int i = 0; i < 2; i++) {
        Aircraft& ac = flights[i];
        switch (ac.state) {
            case PARKED_AT_GATE:
            case BOARDING:
            case DEBOARDING:
                ac.speed = 0.0f;
                ac.pitch = 0.0f;
                break;

            case TAXI_TO_RUNWAY:
                if (!isRunwayClear(i) && ac.x > -2.5f) break; // Hold short if runway busy
                if (ac.x < 0.0f) {
                    ac.x += 0.04f;
                    ac.yaw = 90.0f;
                } else {
                    ac.x = 0.0f;
                    if (ac.yaw > 0.0f) ac.yaw -= 2.0f;
                    else { ac.yaw = 0.0f; ac.state = TAKEOFF_ROLL; }
                }
                break;

            case TAKEOFF_ROLL:
                ac.speed += 0.004f;
                ac.z += ac.speed;
                if (ac.speed >= 0.24f) ac.state = CLIMB;
                break;

            case CLIMB:
                if (ac.pitch < 18.0f) ac.pitch += 0.4f;
                ac.z += ac.speed;
                ac.y += ac.speed * sin(ac.pitch * 3.14159f / 180.0f);
                if (ac.y >= 19.0f) {
                    ac.state = IN_AIR;
                }
                break;

            case IN_AIR:
                // Ready in sky whenever user presses 'L' to land this aircraft
                break;

            case APPROACH:
                ac.pitch = -7.0f;
                ac.z += 0.22f;
                if (ac.y > 0.85f) ac.y -= 0.068f;
                if (ac.y <= 0.85f) {
                    ac.y = 0.85f;
                    ac.state = TOUCHDOWN;
                }
                break;

            case TOUCHDOWN:
                if (ac.pitch < 0.0f) ac.pitch += 0.5f;
                else ac.pitch = 0.0f;
                if (ac.speed > 0.05f) {
                    ac.speed -= 0.0035f;
                    ac.z += ac.speed;
                } else {
                    ac.state = TAXI_TO_GATE;
                }
                break;

            case TAXI_TO_GATE:
                // Taxi along runway to assigned gate Z, then turn into gate X
                if (fabs(ac.z - ac.gateZ) > 0.12f) {
                    if (ac.z < ac.gateZ) { ac.yaw = 0.0f;   ac.z += 0.07f; }
                    else                 { ac.yaw = 180.0f; ac.z -= 0.07f; }
                } else {
                    ac.z = ac.gateZ;
                    if (ac.x > ac.gateX) {
                        ac.yaw = -90.0f;
                        ac.x -= 0.05f;
                    } else {
                        ac.x = ac.gateX;
                        if (ac.yaw < 90.0f) ac.yaw += 3.0f;
                        else {
                            ac.yaw = 90.0f;
                            // Automatically open door & start Deboarding passengers!
                            ac.passengersProcessed = 0;
                            paxSpawnTimer = 0.5f;
                            ac.state = DEBOARDING;
                        }
                    }
                }
                break;
        }
    }

    updateWindowTitle();
}

void setupLighting() {
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);

    GLfloat matSpecular[]  = { 0.40f, 0.40f, 0.40f, 1.0f };
    GLfloat lightPos[]     = { 24.0f, 27.0f, 38.0f, 1.0f };
    GLfloat dayDiffuse[]   = { 0.98f, 0.96f, 0.90f, 1.0f };
    GLfloat nightDiffuse[] = { 0.42f, 0.48f, 0.68f, 1.0f };
    GLfloat ambientLight[] = { isNight ? 0.30f : 0.38f, isNight ? 0.32f : 0.40f, isNight ? 0.44f : 0.42f, 1.0f };

    glMaterialfv(GL_FRONT, GL_SPECULAR, matSpecular);
    glMaterialf(GL_FRONT, GL_SHININESS, 36.0f);
    glLightfv(GL_LIGHT0, GL_POSITION, lightPos);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, isNight ? nightDiffuse : dayDiffuse);
    glLightfv(GL_LIGHT0, GL_AMBIENT, ambientLight);

    if (isNight) {
        glEnable(GL_LIGHT1);
        GLfloat apronLightPos[] = { -7.0f, 6.0f, -2.0f, 1.0f };
        GLfloat warmFlood[]     = { 0.65f, 0.58f, 0.40f, 1.0f };
        glLightfv(GL_LIGHT1, GL_POSITION, apronLightPos);
        glLightfv(GL_LIGHT1, GL_DIFFUSE, warmFlood);
        glClearColor(0.07f, 0.11f, 0.24f, 1.0f);
    } else {
        glDisable(GL_LIGHT1);
        glClearColor(0.45f, 0.76f, 0.96f, 1.0f);
    }
}

void renderScene() {
    setupLighting();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();

    float radY = camAngleY * 3.14159f / 180.0f;
    float radX = camAngleX * 3.14159f / 180.0f;
    float eyeX = camDist * cos(radX) * sin(radY);
    float eyeY = camDist * sin(radX);
    float eyeZ = camDist * cos(radX) * cos(radY);

    gluLookAt(eyeX, eyeY, eyeZ,
              -2.5f, 1.5f, -2.0f,
              0.0f, 1.0f, 0.0f);

    drawEnvironment();
    drawClouds();
    drawBuildings();
    drawGroundVehicle();

    // Draw Boarding Stairs next to whichever plane is currently boarding or deboarding
    for (int i = 0; i < 2; i++) {
        if (flights[i].state == BOARDING || flights[i].state == DEBOARDING) {
            drawBoardingStairs(flights[i].gateX, flights[i].gateZ);
        }
    }

    drawAllPassengers();

    // Draw Both Aircraft (unless high off-screen in IN_AIR state)
    for (int i = 0; i < 2; i++) {
        if (flights[i].state != IN_AIR) {
            drawAirplaneModel(flights[i], i == selectedFlight);
        }
    }
}

void setupProjection(int w, int h) {
    if (h == 0) h = 1;
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0f, (double)w / (double)h, 0.1, 250.0);
    glMatrixMode(GL_MODELVIEW);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            setupProjection(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_KEYDOWN:
            // Camera Controls
            if (wParam == VK_LEFT)  camAngleY -= 4.0f;
            if (wParam == VK_RIGHT) camAngleY += 4.0f;
            if (wParam == VK_UP)    camAngleX += 4.0f;
            if (wParam == VK_DOWN)  camAngleX -= 4.0f;
            if (wParam == 'W')      camDist   -= 1.5f;
            if (wParam == 'S')      camDist   += 1.5f;
            if (camAngleX > 85.0f) camAngleX = 85.0f;
            if (camAngleX < 5.0f)  camAngleX = 5.0f;
            if (camDist < 5.0f)    camDist   = 5.0f;

            // Select which Aircraft to Command ('1' = BG-101, '2' = EK-582)
            if (wParam == '1') selectedFlight = 0;
            if (wParam == '2') selectedFlight = 1;

            // 'T' = Board Passengers & Takeoff for the SELECTED aircraft (works anytime it's parked at gate!)
            if (wParam == 'T') {
                Aircraft& ac = flights[selectedFlight];
                if (ac.state == PARKED_AT_GATE) {
                    ac.passengersProcessed = 0;
                    paxSpawnTimer = 0.5f;
                    ac.state = BOARDING;
                }
            }

            // 'L' = Land the SELECTED aircraft (works anytime it's in the air & runway is clear!)
            if (wParam == 'L') {
                Aircraft& ac = flights[selectedFlight];
                if (ac.state == IN_AIR && isRunwayClear(selectedFlight)) {
                    ac.x = 0.0f;
                    ac.y = 15.0f;
                    ac.z = -30.0f;
                    ac.yaw = 0.0f;
                    ac.pitch = -7.0f;
                    ac.speed = 0.23f;
                    ac.state = APPROACH;
                }
            }

            if (wParam == 'N') isNight = !isNight;
            if (wParam == VK_SPACE) isPaused = !isPaused;
            return 0;
        case WM_CLOSE:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "SkyPort3DClass";
    wc.style = CS_OWNDC;
    RegisterClass(&wc);

    g_hwnd = CreateWindow("SkyPort3DClass",
                          "SkyPort 3D - Interactive Multi-Flight Simulation",
                          WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                          60, 40, 1180, 760, NULL, NULL, hInst, NULL);

    HDC hdc = GetDC(g_hwnd);
    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                  PFD_TYPE_RGBA, 32, 0,0,0,0,0,0,0,0,0,0,0,0,0, 24, 8, 0, PFD_MAIN_PLANE, 0,0,0,0 };
    SetPixelFormat(hdc, ChoosePixelFormat(hdc, &pfd), &pfd);
    HGLRC hrc = wglCreateContext(hdc);
    wglMakeCurrent(hdc, hrc);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_NORMALIZE);
    setupProjection(1180, 760);

    MSG msg;
    while (true) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        updateSimulation();
        renderScene();
        SwapBuffers(hdc);
        Sleep(16);
    }
}
