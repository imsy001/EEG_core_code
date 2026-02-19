/* main.cpp - GUI entry */

#include <iostream>
#include "eeg_gui.hpp"

int main() {
    try {
        EEGGuiApp app;
        return app.run();
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << "\n";
        return 2;
    } catch (...) {
        std::cerr << "[FATAL] unknown error\n";
        return 2;
    }
}
