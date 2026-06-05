#pragma once
#include <vector>
#include <string>
#include <cstdint>

// Un événement MIDI dans un clip
struct MidiEvent {
    double beat;          // Position en temps musical (en beats)
    uint8_t data[3];      // Les octets MIDI (status, note, velocity)
    uint8_t size;         // Taille du message (1, 2 ou 3)
};

// Un clip MIDI complet
struct MidiClip {
    std::string name;
    std::vector<MidiEvent> events;
    double length_beats;  // Durée totale en beats (ex: 4.0 = 1 mesure en 4/4)
    bool loaded = false;
};