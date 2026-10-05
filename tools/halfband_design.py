#!/usr/bin/env python3
"""The decimator's coefficients (dsp/halfband.h): a polyphase IIR halfband, Laurent de Soras's HIIR
design (PolyphaseIir2Designer: an elliptic halfband split into two chains of allpasses).

    python3 tools/halfband_design.py [coefficients] [passband edge Hz] [high rate Hz]

Prints the coefficients, the passband ripple and the stopband attenuation for the design; the
engine uses 8 coefficients, a 20 kHz passband at 88.2 kHz (stopband from 24.1 kHz, >= 85 dB).
"""
import cmath
import math
import sys


def transition_params(tbw):
    k = math.tan((1 - tbw * 2) * math.pi / 4) ** 2
    kk = (1 - k * k) ** 0.25
    e = 0.5 * (1 - kk) / (1 + kk)
    e4 = e ** 4
    return k, e * (1 + e4 * (2 + e4 * (15 + 150 * e4)))


def _acc(q, order, c, num):
    i, j, acc = (0, 1, 0.0) if num else (1, -1, 0.0)
    while True:
        if num:
            t = q ** (i * (i + 1)) * math.sin((i * 2 + 1) * c * math.pi / order) * j
        else:
            t = q ** (i * i) * math.cos(i * 2 * c * math.pi / order) * j
        acc += t
        j, i = -j, i + 1
        if abs(t) < 1e-100:
            return acc


def coefficients(n, tbw):
    k, q = transition_params(tbw)
    order = 2 * n + 1
    out = []
    for c in range(1, n + 1):
        ww = _acc(q, order, c, True) * q ** 0.25 / (_acc(q, order, c, False) + 0.5)
        w2 = ww * ww
        x = math.sqrt((1 - w2 * k) * (1 - w2 / k)) / (1 + w2)
        out.append((1 - x) / (1 + x))
    return out


def response(cs, f):
    """|H| at f (fraction of the high rate): 0.5 (A0(z^2) + z^-1 A1(z^2)), A0 the even coefficients."""
    z = cmath.exp(2j * math.pi * f)
    a0 = a1 = 1
    for i, c in enumerate(cs):
        ap = (c + z ** -2) / (1 + c * z ** -2)
        if i % 2 == 0:
            a0 *= ap
        else:
            a1 *= ap
    return abs(0.5 * (a0 + a1 / z))


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    edge = float(sys.argv[2]) if len(sys.argv) > 2 else 20000.0
    rate = float(sys.argv[3]) if len(sys.argv) > 3 else 88200.0
    tbw = 0.25 - edge / rate
    cs = coefficients(n, tbw)
    grid = [i / 400 for i in range(401)]
    ripple = max(abs(20 * math.log10(response(cs, g * (0.25 - tbw)))) for g in grid)
    stop = min(-20 * math.log10(response(cs, 0.25 + tbw + g * (0.25 - tbw)) + 1e-30) for g in grid)
    print("%d coefficients, transition %.4f of %.0f Hz: passband ripple %.1e dB to %.0f Hz, stopband %.1f dB from %.0f Hz"
          % (n, tbw, rate, ripple, edge, stop, (0.25 + tbw) * rate))
    print("{" + ", ".join("%.10ff" % c for c in cs) + "}")


if __name__ == "__main__":
    main()
