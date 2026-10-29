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
    // The original's hinge points (the 0x00037100 mapping chunk: 16 matrices of 80 bytes from +0x18, four groups of four, the
    // translation row of the first of each group = hood, hood (again), right door, left door).  part: 1 hood, 3 left door, 4 right door.
    bool Hinge(int part, float out[3]) const;

private:
    std::vector<Clip> m_clips;
    float m_hinge[5][3] = {};
    bool m_haveHinge[5] = {};
};
