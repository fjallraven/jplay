#include "App.h"

#include <SDL3/SDL_main.h>

int main(int argc, char** argv) {
    App app;
    if (!app.init(argc, argv))
        return 1;
    app.run();
    return 0;
}
