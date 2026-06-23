# SPDX-License-Identifier: MIT
# Generate config.hpp into the build include tree as shadertoy/config.hpp so
# that <shadertoy/config.hpp> resolves from both the build and install trees.

configure_file(
    input:         'config.hpp.in',
    output:        'config.hpp',
    configuration: conf,
    install:       true,
    install_dir:   get_option('includedir') / 'shadertoy',
)
