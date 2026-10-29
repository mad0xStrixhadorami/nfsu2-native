#pragma once
// The hood / boot / door animations of CARS\<car>\PARTS_ANIMATIONS.BIN (ISSUES #358): 21 EAGL animation banks per car, each a short
// rotation curve of one panel about its hinge.  Decoded: tools/car_part_anims.py documents the container.
#include <cstdint>
#include <string>
#include <vector>

class CarPartAnims {
public:
    struct Clip {
        std::string name;                          // e.g. SKYLINE_ZAM_HOOD_FRONT_q
        std::vector<float> angle;                  // radians per key (30 keys per second), signed by the hinge axis
        float axisSign = 1.0f;
        float Duration() const { return angle.size() > 1 ? float(angle.size() - 1) / 30.0f : 0.0f; }
        // The panel's opening angle (radians) at `t` seconds into the clip: linear between keys, held after the last one.
        float At(float t) const;
    };
    bool Load(const std::vector<uint8_t> &file);
    const Clip *Find(const char *part) const; // by the name between "ZAM_" and "_q": HOOD_FRONT, TRUNK_REG, LEFT_DOOR_REG, ...
    size_t Count() const { return m_clips.size(); }

private:
    std::vector<Clip> m_clips;
};
