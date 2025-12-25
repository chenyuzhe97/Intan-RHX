#pragma once
#include <QtGlobal>
#include <cmath>

struct Biquad {
    // Direct Form I
    double b0=1, b1=0, b2=0;
    double a0=1, a1=0, a2=0;
    double z1=0, z2=0;

    void reset(){ z1 = z2 = 0; }

    inline float process(float x){
        double y = (b0/a0)*x + z1;
        z1 = (b1/a0)*x - (a1/a0)*y + z2;
        z2 = (b2/a0)*x - (a2/a0)*y;
        return (float)y;
    }

    // RBJ cookbook
    static Biquad lowpass(double fs, double fc, double Q=0.707){
        fc = qBound(1.0, fc, fs*0.49);
        double w0 = 2*M_PI*fc/fs;
        double cs = std::cos(w0), sn = std::sin(w0);
        double alpha = sn/(2*Q);

        Biquad f;
        f.b0 = (1 - cs)/2;
        f.b1 = 1 - cs;
        f.b2 = (1 - cs)/2;
        f.a0 = 1 + alpha;
        f.a1 = -2*cs;
        f.a2 = 1 - alpha;
        return f;
    }

    static Biquad highpass(double fs, double fc, double Q=0.707){
        fc = qBound(1.0, fc, fs*0.49);
        double w0 = 2*M_PI*fc/fs;
        double cs = std::cos(w0), sn = std::sin(w0);
        double alpha = sn/(2*Q);

        Biquad f;
        f.b0 = (1 + cs)/2;
        f.b1 = -(1 + cs);
        f.b2 = (1 + cs)/2;
        f.a0 = 1 + alpha;
        f.a1 = -2*cs;
        f.a2 = 1 - alpha;
        return f;
    }

    // Bandpass (constant skirt gain, peak gain = Q)
    static Biquad bandpass(double fs, double fc, double Q){
        fc = qBound(1.0, fc, fs*0.49);
        Q  = qMax(0.05, Q);
        double w0 = 2*M_PI*fc/fs;
        double cs = std::cos(w0), sn = std::sin(w0);
        double alpha = sn/(2*Q);

        Biquad f;
        f.b0 = alpha;
        f.b1 = 0;
        f.b2 = -alpha;
        f.a0 = 1 + alpha;
        f.a1 = -2*cs;
        f.a2 = 1 - alpha;
        return f;
    }

    static Biquad notch(double fs, double f0, double Q=30.0){
        f0 = qBound(1.0, f0, fs*0.49);
        Q  = qMax(1.0, Q);
        double w0 = 2*M_PI*f0/fs;
        double cs = std::cos(w0), sn = std::sin(w0);
        double alpha = sn/(2*Q);

        Biquad f;
        f.b0 = 1;
        f.b1 = -2*cs;
        f.b2 = 1;
        f.a0 = 1 + alpha;
        f.a1 = -2*cs;
        f.a2 = 1 - alpha;
        return f;
    }
};
