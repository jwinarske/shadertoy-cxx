# Bundled shaders

shadertoy-cxx bundles only shaders that are license-compatible with this MIT
project:

| File | Source | Name | Author | License |
|------|--------|------|--------|---------|
| `default.frag` | original | kaleidoscope | shadertoy-cxx | MIT |
| `trail.json` | original | Feedback Trail (2-pass) | shadertoy-cxx | MIT |
| `X3yXRd.json` | [shadertoy.com/view/X3yXRd](https://www.shadertoy.com/view/X3yXRd) | Silky Carbon Fabric | Giorgi Azmaipharashvili | MIT (notice retained in the shader) |

## Running other Shadertoy shaders

Most Shadertoy shaders are under **CC BY-NC-SA 3.0** (the site default) or another
non-permissive license, so they are **not redistributed here**. You can still run
any of them locally — fetch one into a directory of your own and point the player
at it:

```sh
scripts/fetch_shadertoy.sh <shaderID> ~/shadertoy
shadertoy_egl ~/shadertoy/<shaderID>.json
```

Fetching requires your own Shadertoy API key (https://www.shadertoy.com/howto#q2).
Shaders you download remain under their authors' licenses; respect those terms.
