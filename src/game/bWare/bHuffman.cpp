// EA "HUFF" (huffcodex, 0x30FB) decoder, ported from the UG2 PS2 binary at 0x002e8720.
// This is a mechanical port of Ghidra's output for that function, kept as is so it can be
// compared line by line with the original; it is verified by decompressing real game data
// (sizes match the HUFF headers and decoded textures are correct). The original reads a global
// (0x00568300) that is always 0 and never written; it is folded to a constant.
// Built only for the 32-bit target: the original code mixes pointers and 32-bit integers.
#include <cstdint>

#include "game/bWare/bCompression.h"

namespace {
typedef uint8_t byte;
typedef uint32_t uint;
typedef uint32_t undefined4;
inline uint CONCAT11(byte a, byte b) { return (uint(a) << 8) | b; }
constexpr uint DAT_00568300 = 0;
} // namespace

uint32_t bHuffDecode(const uint8_t *src, uint8_t *dst);

static uint HuffDecodeImpl(byte *param_1,byte *param_2)

{
  byte bVar1;
  undefined4 *puVar2;
  uint *puVar3;
  byte bVar4;
  char cVar5;
  uint uVar6;
  uint uVar7;
  int iVar8;
  uint uVar9;
  uint uVar10;
  int iVar11;
  int iVar12;
  uint uVar13;
  uint uVar14;
  uint uVar15;
  int iVar16;
  uint uVar17;
  byte *pbVar18;
  int iVar19;
  byte *pbVar20;
  byte *pbVar21;
  int iVar22;
  uint uVar23;
  // Ghidra split the original stack frame into separate arrays, but the code indexes across
  // them (e.g. a sentinel written one past auStack_510). Recreate the frame as one block with
  // each array at its original offset (name = offset from the frame top at 0x590).
  alignas(16) byte frame_[0x590 - 0xd0] = {};
  uint *auStack_590 = reinterpret_cast<uint *>(frame_ + (0x590 - 0x590));
  int *aiStack_550 = reinterpret_cast<int *>(frame_ + (0x590 - 0x550));
  uint *auStack_510 = reinterpret_cast<uint *>(frame_ + (0x590 - 0x510));
  byte *abStack_4d0 = frame_ + (0x590 - 0x4d0);
  byte *abStack_3d0 = frame_ + (0x590 - 0x3d0);
  byte *abStack_2d0 = frame_ + (0x590 - 0x2d0);
  undefined4 *auStack_2cc = reinterpret_cast<undefined4 *>(frame_ + (0x590 - 0x2cc));
  undefined4 *auStack_1d0 = reinterpret_cast<undefined4 *>(frame_ + (0x590 - 0x1d0));
  (void)auStack_2cc;
  byte *pbStack_d0;
  uint uStack_cc;
  uint uStack_c8;
  uint uStack_c4;
  uint uStack_c0;
  uint uStack_bc;
  uint uStack_b8;
  uint uStack_b4;
  byte *pbStack_b0;
  int *piStack_ac;
  undefined4 *puStack_a8;
  uint *puStack_a4;
  byte *pbStack_a0;
  uint uStack_9c;
  
  uVar10 = DAT_00568300;
  pbStack_d0 = param_2;
  uVar17 = 0;
  uVar14 = 0;
  uStack_c8 = 0;
  if (param_1 == (byte *)0x0) {
    return 0;
  }
  iVar11 = -0x10;
  uStack_c4 = DAT_00568300;
  if (DAT_00568300 != 0) {
    iVar11 = -0x10 - DAT_00568300;
  }
  uVar15 = 0;
  if (iVar11 < 0) {
    bVar4 = *param_1;
    uVar15 = -iVar11;
    pbVar18 = param_1 + 1;
    iVar11 = iVar11 + 0x10;
    param_1 = param_1 + 2;
    uVar14 = (uint)CONCAT11(bVar4,*pbVar18);
    uVar15 = uVar14 << (uVar15 & 0x1f);
  }
  uVar23 = uVar15 >> 0x10;
  iVar12 = iVar11 + -0x10;
  uVar15 = uVar15 << 0x10;
  if (iVar12 < 0) {
    bVar4 = *param_1;
    pbVar18 = param_1 + 1;
    param_1 = param_1 + 2;
    uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
    uVar15 = uVar14 << (-iVar12 & 0x1fU);
    iVar12 = iVar11;
  }
  if ((uVar23 & 0x8000) == 0) {
    if ((uVar23 & 0x100) != 0) {
      iVar11 = iVar12 + -8;
      iVar16 = uVar15 << 8;
      if (iVar11 < 0) {
        bVar4 = *param_1;
        pbVar18 = param_1 + 1;
        uVar15 = -iVar11;
        param_1 = param_1 + 2;
        iVar11 = iVar12 + 8;
        uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
        iVar16 = uVar14 << (uVar15 & 0x1f);
      }
      iVar12 = iVar11 + -0x10;
      uVar15 = iVar16 << 0x10;
      if (iVar12 < 0) {
        bVar4 = *param_1;
        pbVar18 = param_1 + 1;
        param_1 = param_1 + 2;
        uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
        uVar15 = uVar14 << (-iVar12 & 0x1fU);
        iVar12 = iVar11;
      }
    }
    uVar6 = uVar15 >> 0x18;
    uVar15 = uVar15 << 8;
    iVar11 = iVar12 + -8;
    if (iVar11 < 0) {
      bVar4 = *param_1;
      pbVar18 = param_1 + 1;
      param_1 = param_1 + 2;
      uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
      uVar15 = uVar14 << (-iVar11 & 0x1fU);
      iVar11 = iVar12 + 8;
    }
  }
  else {
    if ((uVar23 & 0x100) != 0) {
      iVar11 = iVar12 + -0x10;
      iVar16 = 0;
      if (iVar11 < 0) {
        bVar4 = *param_1;
        pbVar18 = param_1 + 1;
        param_1 = param_1 + 2;
        uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
        iVar16 = uVar14 << (-iVar11 & 0x1fU);
        iVar11 = iVar12;
      }
      iVar12 = iVar11 + -0x10;
      uVar15 = iVar16 << 0x10;
      if (iVar12 < 0) {
        bVar4 = *param_1;
        pbVar18 = param_1 + 1;
        param_1 = param_1 + 2;
        uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
        uVar15 = uVar14 << (-iVar12 & 0x1fU);
        iVar12 = iVar11;
      }
    }
    uVar6 = uVar15 >> 0x10;
    uVar15 = uVar15 << 0x10;
    iVar11 = iVar12 + -0x10;
    if (iVar11 < 0) {
      bVar4 = *param_1;
      pbVar18 = param_1 + 1;
      param_1 = param_1 + 2;
      uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
      uVar15 = uVar14 << (-iVar11 & 0x1fU);
      iVar11 = iVar12;
    }
  }
  uStack_cc = uVar15 >> 0x10;
  iVar12 = iVar11 + -0x10;
  uVar23 = uVar23 & 0xfffffeff;
  uVar15 = uVar15 << 0x10;
  if (iVar12 < 0) {
    bVar4 = *param_1;
    pbVar18 = param_1 + 1;
    param_1 = param_1 + 2;
    uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
    uVar15 = uVar14 << (-iVar12 & 0x1fU);
    iVar12 = iVar11;
  }
  uStack_cc = uStack_cc | uVar6 << 0x10;
  uVar6 = uVar15 >> 0x18;
  iVar11 = iVar12 + -8;
  uVar15 = uVar15 << 8;
  if (iVar11 < 0) {
    bVar4 = *param_1;
    pbVar18 = param_1 + 1;
    uVar15 = -iVar11;
    param_1 = param_1 + 2;
    iVar11 = iVar12 + 8;
    uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
    uVar15 = uVar14 << (uVar15 & 0x1f);
  }
  uStack_bc = 0x20 - DAT_00568300;
  piStack_ac = aiStack_550;
  iVar22 = 0;
  puStack_a4 = auStack_510;
  puStack_a8 = auStack_1d0;
  pbStack_a0 = abStack_4d0;
  iVar16 = 0;
  uStack_c0 = DAT_00568300;
  iVar12 = 1;
  do {
    iVar8 = iVar12;
    aiStack_550[iVar8] = iVar16 * 2 - iVar22;
    if ((int)uVar15 < 0) {
      uVar17 = uVar15 >> 0x1d;
      iVar12 = iVar11 + -3;
      uVar15 = uVar15 << 3;
      if (iVar12 < 0) {
        bVar4 = *param_1;
        pbVar18 = param_1 + 1;
        param_1 = param_1 + 2;
        uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
        uVar15 = uVar14 << (-iVar12 & 0x1fU);
        iVar12 = iVar11 + 0xd;
      }
    }
    else {
      if (uVar15 >> 0x10 == 0) {
        uVar9 = 2;
        do {
          uVar17 = -((int)uVar15 >> 0x1f);
          uVar9 = uVar9 + 1;
          iVar12 = iVar11 + -1;
          uVar15 = uVar15 << 1;
          if (iVar12 < 0) {
            bVar4 = *param_1;
            pbVar18 = param_1 + 1;
            param_1 = param_1 + 2;
            uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
            uVar15 = uVar14 << (-iVar12 & 0x1fU);
            iVar12 = iVar11 + 0xf;
          }
          iVar11 = iVar12;
        } while (uVar17 == 0);
      }
      else {
        uVar9 = 2;
        do {
          uVar7 = uVar15;
          uVar15 = uVar7 << 1;
          uVar9 = uVar9 + 1;
        } while (-1 < (int)uVar15);
        iVar11 = (iVar11 + 1) - uVar9;
        uVar15 = uVar7 << 2;
        if (uStack_c0 != 0) {
          iVar11 = iVar11 - DAT_00568300;
          uVar17 = uVar15 >> (uStack_bc & 0x1f);
          uVar15 = uVar15 << (DAT_00568300 & 0x1f);
        }
        if (iVar11 < 0) {
          bVar4 = *param_1;
          pbVar18 = param_1 + 1;
          uVar15 = -iVar11;
          param_1 = param_1 + 2;
          iVar11 = iVar11 + 0x10;
          uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
          uVar15 = uVar14 << (uVar15 & 0x1f);
        }
      }
      if ((int)uVar9 < 0x11) {
        if (uVar9 != 0) {
          iVar11 = iVar11 - uVar9;
          uVar17 = uVar15 >> (0x20 - uVar9 & 0x1f);
          uVar15 = uVar15 << (uVar9 & 0x1f);
        }
        if (iVar11 < 0) {
          bVar4 = *param_1;
          pbVar18 = param_1 + 1;
          uVar15 = -iVar11;
          param_1 = param_1 + 2;
          iVar11 = iVar11 + 0x10;
          uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
          uVar15 = uVar14 << (uVar15 & 0x1f);
        }
        uVar17 = uVar17 + (1 << (uVar9 & 0x1f));
        iVar12 = iVar11;
      }
      else {
        if (uVar9 != 0x10) {
          uVar17 = uVar15 >> (0x30 - uVar9 & 0x1f);
          uVar15 = uVar15 << (uVar9 - 0x10 & 0x1f);
          iVar11 = (iVar11 + 0x10) - uVar9;
        }
        if (iVar11 < 0) {
          bVar4 = *param_1;
          pbVar18 = param_1 + 1;
          uVar15 = -iVar11;
          param_1 = param_1 + 2;
          iVar11 = iVar11 + 0x10;
          uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
          uVar15 = uVar14 << (uVar15 & 0x1f);
        }
        uVar7 = uVar15 >> 0x10;
        iVar12 = iVar11 + -0x10;
        uVar15 = uVar15 << 0x10;
        if (iVar12 < 0) {
          bVar4 = *param_1;
          pbVar18 = param_1 + 1;
          param_1 = param_1 + 2;
          uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
          uVar15 = uVar14 << (-iVar12 & 0x1fU);
          iVar12 = iVar11;
        }
        uVar17 = (uVar7 | uVar17 << 0x10) + (1 << (uVar9 & 0x1f));
      }
    }
    iVar11 = iVar12;
    uVar17 = uVar17 - 4;
    iVar22 = iVar22 + uVar17;
    auStack_590[iVar8] = uVar17;
    iVar16 = iVar16 * 2 + uVar17;
    uVar9 = 0;
    if (uVar17 != 0) {
      uVar9 = iVar16 << (0x10U - iVar8 & 0x1f) & 0xffff;
    }
    auStack_510[iVar8] = uVar9;
    iVar12 = iVar8 + 1;
  } while ((uVar17 == 0) || (uVar9 != 0));
  uStack_9c = (uint)(iVar8 < 1);
  puStack_a4[iVar8] = 0xffffffff;
  iVar12 = 0x10;
  puVar2 = puStack_a8;
  do {
    *puVar2 = 0;
    iVar12 = iVar12 + -1;
    puVar2[1] = 0;
    puVar2[2] = 0;
    puVar2[3] = 0;
    puVar2 = puVar2 + 4;
  } while (iVar12 != 0);
  uVar17 = 0xff;
  if (0 < iVar22) {
    uStack_b8 = uStack_c4;
    uStack_b4 = 0x20 - uVar10;
    pbStack_b0 = pbStack_a0;
    iVar12 = 0;
    do {
      uVar9 = 0;
      if ((int)uVar15 < 0) {
        uVar9 = uVar15 >> 0x1d;
        iVar16 = iVar11 + -3;
        uVar15 = uVar15 << 3;
        if (iVar16 < 0) {
          bVar4 = *param_1;
          pbVar18 = param_1 + 1;
          param_1 = param_1 + 2;
          uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
          uVar15 = uVar14 << (-iVar16 & 0x1fU);
          iVar16 = iVar11 + 0xd;
        }
      }
      else {
        if (uVar15 >> 0x10 == 0) {
          uVar7 = 2;
          do {
            uVar9 = -((int)uVar15 >> 0x1f);
            uVar7 = uVar7 + 1;
            iVar16 = iVar11 + -1;
            uVar15 = uVar15 << 1;
            if (iVar16 < 0) {
              bVar4 = *param_1;
              pbVar18 = param_1 + 1;
              param_1 = param_1 + 2;
              uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
              uVar15 = uVar14 << (-iVar16 & 0x1fU);
              iVar16 = iVar11 + 0xf;
            }
            iVar11 = iVar16;
          } while (uVar9 == 0);
        }
        else {
          uVar7 = 2;
          do {
            uVar13 = uVar15;
            uVar15 = uVar13 << 1;
            uVar7 = uVar7 + 1;
          } while (-1 < (int)uVar15);
          iVar11 = (iVar11 + 1) - uVar7;
          uVar15 = uVar13 << 2;
          if (uStack_c4 != 0) {
            iVar11 = iVar11 - uVar10;
            uVar9 = uVar15 >> (0x20 - uVar10 & 0x1f);
            uVar15 = uVar15 << (uVar10 & 0x1f);
          }
          if (iVar11 < 0) {
            bVar4 = *param_1;
            pbVar18 = param_1 + 1;
            uVar15 = -iVar11;
            param_1 = param_1 + 2;
            iVar11 = iVar11 + 0x10;
            uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
            uVar15 = uVar14 << (uVar15 & 0x1f);
          }
        }
        if ((int)uVar7 < 0x11) {
          if (uVar7 != 0) {
            iVar11 = iVar11 - uVar7;
            uVar9 = uVar15 >> (0x20 - uVar7 & 0x1f);
            uVar15 = uVar15 << (uVar7 & 0x1f);
          }
          if (iVar11 < 0) {
            bVar4 = *param_1;
            pbVar18 = param_1 + 1;
            uVar15 = -iVar11;
            param_1 = param_1 + 2;
            iVar11 = iVar11 + 0x10;
            uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
            uVar15 = uVar14 << (uVar15 & 0x1f);
          }
          uVar9 = uVar9 + (1 << (uVar7 & 0x1f));
          iVar16 = iVar11;
        }
        else {
          if (uVar7 != 0x10) {
            iVar11 = (iVar11 + 0x10) - uVar7;
            uVar9 = uVar15 >> (0x30 - uVar7 & 0x1f);
            uVar15 = uVar15 << (uVar7 - 0x10 & 0x1f);
          }
          if (iVar11 < 0) {
            bVar4 = *param_1;
            pbVar18 = param_1 + 1;
            uVar15 = -iVar11;
            param_1 = param_1 + 2;
            iVar11 = iVar11 + 0x10;
            uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
            uVar15 = uVar14 << (uVar15 & 0x1f);
          }
          uVar13 = uVar15 >> 0x10;
          iVar16 = iVar11 + -0x10;
          uVar15 = uVar15 << 0x10;
          if (iVar16 < 0) {
            bVar4 = *param_1;
            pbVar18 = param_1 + 1;
            param_1 = param_1 + 2;
            uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
            uVar15 = uVar14 << (-iVar16 & 0x1fU);
            iVar16 = iVar11;
          }
          uVar9 = (uVar13 | uVar9 << 0x10) + (1 << (uVar7 & 0x1f));
        }
      }
      iVar11 = iVar16;
      iVar19 = iVar12 + 1;
      iVar16 = uVar9 - 3;
      do {
        uVar9 = uVar17 + 1;
        uVar17 = uVar9 & 0xff;
        if (*(char *)((int)puStack_a8 + uVar17) == '\0') {
          iVar16 = iVar16 + -1;
        }
      } while (iVar16 != 0);
      *(char *)((int)puStack_a8 + uVar17) = '\x01';
      pbStack_a0[iVar12] = (byte)uVar9;
      iVar12 = iVar19;
    } while (iVar19 < iVar22);
  }
  iVar12 = 0x10;
  uVar17 = uVar15 >> 0x18;
  pbVar18 = abStack_2d0;
  do {
    pbVar18[0] = 0x40;
    pbVar18[1] = 0x40;
    pbVar18[2] = 0x40;
    pbVar18[3] = 0x40;
    iVar12 = iVar12 + -1;
    pbVar18[4] = 0x40;
    pbVar18[5] = 0x40;
    pbVar18[6] = 0x40;
    pbVar18[7] = 0x40;
    pbVar18[8] = 0x40;
    pbVar18[9] = 0x40;
    pbVar18[10] = 0x40;
    pbVar18[0xb] = 0x40;
    pbVar18[0xc] = 0x40;
    pbVar18[0xd] = 0x40;
    pbVar18[0xe] = 0x40;
    pbVar18[0xf] = 0x40;
    pbVar18 = pbVar18 + 0x10;
  } while (iVar12 != 0);
  uVar10 = 1;
  pbVar18 = abStack_2d0;
  pbVar20 = abStack_3d0;
  pbVar21 = pbStack_a0;
  if (uStack_9c == 0) {
    do {
      iVar12 = auStack_590[1] - 1;
      iVar16 = 1 << (8 - uVar10 & 0x1f);
      if (iVar12 != -1) {
        bVar4 = *pbVar21;
        while( true ) {
          pbVar21 = pbVar21 + 1;
          uVar9 = uVar10;
          if (bVar4 == uVar6) {
            uStack_c8 = uVar10;
            uVar9 = 0x60;
          }
          iVar22 = 0;
          if (0 < iVar16) {
            do {
              *pbVar20 = bVar4;
              iVar22 = iVar22 + 1;
              *pbVar18 = (byte)uVar9;
              pbVar20 = pbVar20 + 1;
              pbVar18 = pbVar18 + 1;
            } while (iVar22 < iVar16);
          }
          iVar12 = iVar12 + -1;
          if (iVar12 == -1) break;
          bVar4 = *pbVar21;
        }
      }
      uVar10 = uVar10 + 1;
    } while (((int)uVar10 <= iVar8) && (auStack_590[1] = auStack_590[uVar10], (int)uVar10 < 9));
  }
LAB_002e91a8:
  do {
    bVar4 = abStack_2d0[uVar17];
    uVar10 = (uint)bVar4;
    iVar11 = iVar11 - (uint)bVar4;
    pbVar20 = abStack_3d0 + uVar17;
    pbVar18 = param_2;
    if (-1 < iVar11) {
      uVar15 = uVar15 << (bVar4 & 0x1f);
      uVar17 = uVar15 >> 0x18;
      *param_2 = *pbVar20;
      bVar4 = abStack_2d0[uVar17];
      uVar10 = (uint)bVar4;
      iVar11 = iVar11 - (uint)bVar4;
      pbVar18 = param_2 + 1;
      if (-1 < iVar11) {
        pbVar20 = abStack_3d0 + uVar17;
        uVar15 = uVar15 << (bVar4 & 0x1f);
        uVar17 = uVar15 >> 0x18;
        *pbVar18 = *pbVar20;
        bVar4 = abStack_2d0[uVar17];
        uVar10 = (uint)bVar4;
        iVar11 = iVar11 - (uint)bVar4;
        pbVar18 = param_2 + 2;
        if (-1 < iVar11) {
          pbVar20 = abStack_3d0 + uVar17;
          uVar15 = uVar15 << (bVar4 & 0x1f);
          uVar17 = uVar15 >> 0x18;
          *pbVar18 = *pbVar20;
          bVar4 = abStack_2d0[uVar17];
          uVar10 = (uint)bVar4;
          iVar11 = iVar11 - (uint)bVar4;
          pbVar18 = param_2 + 3;
          if (-1 < iVar11) {
            pbVar20 = abStack_3d0 + uVar17;
            uVar15 = uVar15 << (bVar4 & 0x1f);
            uVar17 = uVar15 >> 0x18;
            *pbVar18 = *pbVar20;
            param_2 = param_2 + 4;
            goto LAB_002e91a8;
          }
        }
      }
    }
    iVar11 = iVar11 + 0x10;
    if (iVar11 < 0) {
      if (uVar10 == 0x60) {
        iVar12 = uStack_c8 << 2;
        uVar17 = uStack_c8;
      }
      else {
        uVar17 = 8;
        puVar3 = puStack_a4 + 8;
        iVar12 = 0x20;
        do {
          puVar3 = puVar3 + 1;
          iVar12 = iVar12 + 4;
          uVar17 = uVar17 + 1;
        } while (*puVar3 <= uVar15 >> 0x10);
      }
      uVar9 = uVar15 >> (0x20 - uVar17 & 0x1f);
      iVar16 = (iVar11 + (uVar10 - 0x10)) - uVar17;
      uVar15 = uVar15 << (uVar17 & 0x1f);
      bVar4 = pbStack_a0[uVar9 - *(int *)((int)piStack_ac + iVar12)];
      if (bVar4 == uVar6) {
        if (iVar16 < 0) {
          bVar1 = *param_1;
          goto LAB_002e9268;
        }
LAB_002e928c:
        uVar10 = 0;
        if (bVar4 == uVar6) {
          if ((int)uVar15 < 0) {
            uVar10 = uVar15 >> 0x1d;
            iVar11 = iVar16 + -3;
            uVar15 = uVar15 << 3;
            if (iVar11 < 0) {
              bVar4 = *param_1;
              pbVar20 = param_1 + 1;
              uVar17 = -iVar11;
              param_1 = param_1 + 2;
              iVar11 = iVar16 + 0xd;
              uVar14 = (uint)*pbVar20 | ((uint)bVar4 | uVar14 << 8) << 8;
              uVar15 = uVar14 << (uVar17 & 0x1f);
            }
          }
          else {
            if (uVar15 >> 0x10 == 0) {
              uVar17 = 2;
              do {
                uVar10 = -((int)uVar15 >> 0x1f);
                uVar17 = uVar17 + 1;
                iVar11 = iVar16 + -1;
                uVar15 = uVar15 << 1;
                if (iVar11 < 0) {
                  bVar4 = *param_1;
                  pbVar20 = param_1 + 1;
                  uVar15 = -iVar11;
                  param_1 = param_1 + 2;
                  iVar11 = iVar16 + 0xf;
                  uVar14 = (uint)*pbVar20 | ((uint)bVar4 | uVar14 << 8) << 8;
                  uVar15 = uVar14 << (uVar15 & 0x1f);
                }
                iVar16 = iVar11;
              } while (uVar10 == 0);
            }
            else {
              uVar17 = 2;
              do {
                uVar9 = uVar15;
                uVar15 = uVar9 << 1;
                uVar17 = uVar17 + 1;
              } while (-1 < (int)uVar15);
              iVar11 = (iVar16 + 1) - uVar17;
              uVar15 = uVar9 << 2;
              if (DAT_00568300 != 0) {
                iVar11 = iVar11 - DAT_00568300;
                uVar10 = uVar15 >> (0x20 - DAT_00568300 & 0x1f);
                uVar15 = uVar15 << (DAT_00568300 & 0x1f);
              }
              if (iVar11 < 0) {
                bVar4 = *param_1;
                pbVar20 = param_1 + 1;
                uVar15 = -iVar11;
                param_1 = param_1 + 2;
                iVar11 = iVar11 + 0x10;
                uVar14 = (uint)*pbVar20 | ((uint)bVar4 | uVar14 << 8) << 8;
                uVar15 = uVar14 << (uVar15 & 0x1f);
              }
            }
            if ((int)uVar17 < 0x11) {
              if (uVar17 != 0) {
                iVar11 = iVar11 - uVar17;
                uVar10 = uVar15 >> (0x20 - uVar17 & 0x1f);
                uVar15 = uVar15 << (uVar17 & 0x1f);
              }
              if (iVar11 < 0) {
                bVar4 = *param_1;
                pbVar20 = param_1 + 1;
                uVar15 = -iVar11;
                param_1 = param_1 + 2;
                iVar11 = iVar11 + 0x10;
                uVar14 = (uint)*pbVar20 | ((uint)bVar4 | uVar14 << 8) << 8;
                uVar15 = uVar14 << (uVar15 & 0x1f);
              }
              uVar10 = uVar10 + (1 << (uVar17 & 0x1f));
            }
            else {
              if (uVar17 != 0x10) {
                uVar10 = uVar15 >> (0x30 - uVar17 & 0x1f);
                iVar11 = (iVar11 + 0x10) - uVar17;
                uVar15 = uVar15 << (uVar17 - 0x10 & 0x1f);
              }
              if (iVar11 < 0) {
                bVar4 = *param_1;
                pbVar20 = param_1 + 1;
                uVar15 = -iVar11;
                param_1 = param_1 + 2;
                iVar11 = iVar11 + 0x10;
                uVar14 = (uint)*pbVar20 | ((uint)bVar4 | uVar14 << 8) << 8;
                uVar15 = uVar14 << (uVar15 & 0x1f);
              }
              uVar9 = uVar15 >> 0x10;
              iVar12 = iVar11 + -0x10;
              uVar15 = uVar15 << 0x10;
              if (iVar12 < 0) {
                bVar4 = *param_1;
                pbVar20 = param_1 + 1;
                param_1 = param_1 + 2;
                uVar14 = (uint)*pbVar20 | ((uint)bVar4 | uVar14 << 8) << 8;
                uVar15 = uVar14 << (-iVar12 & 0x1fU);
                iVar12 = iVar11;
              }
              iVar11 = iVar12;
              uVar10 = (uVar9 | uVar10 << 0x10) + (1 << (uVar17 & 0x1f));
            }
          }
          if (uVar10 - 4 != 0) {
            bVar4 = pbVar18[-1];
            uVar17 = uVar15 >> 0x18;
            param_2 = pbVar18;
            do {
              *param_2 = bVar4;
              param_2 = param_2 + 1;
            } while (param_2 < pbVar18 + (uVar10 - 4));
            goto LAB_002e91a8;
          }
          iVar12 = iVar11 + -1;
          iVar16 = uVar15 << 1;
          if (iVar12 < 0) {
            bVar4 = *param_1;
            pbVar20 = param_1 + 1;
            uVar17 = -iVar12;
            param_1 = param_1 + 2;
            iVar12 = iVar11 + 0xf;
            uVar14 = (uint)*pbVar20 | ((uint)bVar4 | uVar14 << 8) << 8;
            iVar16 = uVar14 << (uVar17 & 0x1f);
          }
          if ((int)uVar15 < 0) {
            if ((uVar23 == 0x32fb) || (uVar23 == 0xb2fb)) {
              pbVar18 = pbStack_d0 + uStack_cc;
              bVar4 = 0;
              for (; pbStack_d0 < pbVar18; pbStack_d0 = pbStack_d0 + 1) {
                bVar4 = bVar4 + *pbStack_d0;
                *pbStack_d0 = bVar4;
              }
              return uStack_cc;
            }
            if ((uVar23 != 0x34fb) && (uVar23 != 0xb4fb)) {
              return uStack_cc;
            }
            cVar5 = '\0';
            pbVar18 = pbStack_d0 + uStack_cc;
            bVar4 = 0;
            for (; pbStack_d0 < pbVar18; pbStack_d0 = pbStack_d0 + 1) {
              cVar5 = cVar5 + *pbStack_d0;
              bVar4 = bVar4 + cVar5;
              *pbStack_d0 = bVar4;
            }
            return uStack_cc;
          }
          iVar11 = iVar12 + -8;
          uVar15 = iVar16 << 8;
          if (iVar11 < 0) {
            bVar4 = *param_1;
            pbVar20 = param_1 + 1;
            uVar17 = -iVar11;
            param_1 = param_1 + 2;
            iVar11 = iVar12 + 8;
            uVar14 = (uint)*pbVar20 | ((uint)bVar4 | uVar14 << 8) << 8;
            uVar15 = uVar14 << (uVar17 & 0x1f);
          }
          *pbVar18 = (byte)((uint)iVar16 >> 0x18);
        }
        else {
          *pbVar18 = bVar4;
          iVar11 = iVar16;
        }
      }
      else {
        if (iVar16 < 0) {
          bVar1 = *param_1;
LAB_002e9268:
          pbVar20 = param_1 + 1;
          uVar17 = -iVar16;
          param_1 = param_1 + 2;
          iVar16 = iVar16 + 0x10;
          uVar14 = (uint)*pbVar20 | ((uint)bVar1 | uVar14 << 8) << 8;
          uVar15 = uVar14 << (uVar17 & 0x1f);
          goto LAB_002e928c;
        }
        *pbVar18 = bVar4;
        iVar11 = iVar16;
      }
      uVar17 = uVar15 >> 0x18;
      param_2 = pbVar18 + 1;
      goto LAB_002e91a8;
    }
    *pbVar18 = abStack_3d0[uVar17];
    param_2 = pbVar18 + 1;
    bVar4 = *param_1;
    pbVar18 = param_1 + 1;
    param_1 = param_1 + 2;
    uVar14 = (uint)*pbVar18 | ((uint)bVar4 | uVar14 << 8) << 8;
    uVar15 = uVar14 << (0x10U - iVar11 & 0x1f);
    uVar17 = uVar15 >> 0x18;
  } while( true );
}


uint32_t bHuffDecode(const uint8_t *src, uint8_t *dst)
{
    return HuffDecodeImpl(const_cast<uint8_t *>(src), dst);
}
