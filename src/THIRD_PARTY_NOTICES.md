# Third-party notices

The Daubechies-8 filter coefficients in `twitter_juniward.cpp` and the
optimized constraint-height-7 STC width-2/width-3 column constants in
`twitter_stc.cpp` are derived from the J-UNIWARD reference implementation
distributed by the Digital Data Embedding Laboratory at Binghamton University.

Authors associated with the reference implementation and papers include
Vojtech Holub, Jessica Fridrich, Tomas Filler, Jan Judas, and Tomas Denemark.
The algorithm names and research citations remain the property of their
respective authors. This implementation does not copy the reference program's
Boost/SSE implementation.

Relevant publications:

- V. Holub, J. Fridrich, T. Denemark, “Universal Distortion Function for
  Steganography in an Arbitrary Domain,” EURASIP Journal on Information
  Security, 2014.
- T. Filler, J. Judas, J. Fridrich, “Minimizing Additive Distortion in
  Steganography Using Syndrome-Trellis Codes,” IEEE TIFS, 2011.
