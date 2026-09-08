/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace nr_isac {

/** NumPy-compatible one-dimensional complex transform.
 * Forward is unnormalised; inverse is divided by N. Arbitrary lengths use Bluestein convolution.
 */
void fft_inplace(std::vector<std::complex<double>>& values, bool inverse);
std::vector<std::complex<double>> fft(const std::vector<std::complex<double>>& values, bool inverse);

} // namespace nr_isac
