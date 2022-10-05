/* Copyright (C) 2021, Nikolai Wuttke. All rights reserved.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "shader_code.hpp"

#include <array>


namespace rigel::renderer
{

namespace
{
#ifdef __vita__
const char* FRAGMENT_SOURCE_SIMPLE = R"shd(
uniform sampler2D textureData;

float4 main(
  float2 texCoordFrag : TEXCOORD0
) {
  return TEXTURE_LOOKUP(textureData, texCoordFrag);
}
)shd";


const char* FRAGMENT_SOURCE = R"shd(
uniform sampler2D textureData;
uniform float4 overlayColor;

uniform float4 colorModulation;
uniform bool enableRepeat;

float4 main(
  float2 texCoordFrag : TEXCOORD0
) {
  float2 texCoords = texCoordFrag;
  if (enableRepeat) {
    texCoords.x = frac(texCoords.x);
    texCoords.y = frac(texCoords.y);
  }

  float4 baseColor = TEXTURE_LOOKUP(textureData, texCoords);
  float4 modulated = baseColor * colorModulation;
  float targetAlpha = modulated.a;

  return
    float4(lerp(modulated.rgb, overlayColor.rgb, overlayColor.a), targetAlpha);
}
)shd";

const char* VERTEX_SOURCE_SOLID = R"shd(
uniform float4x4 transform;

void main(
  float2 position,
  float4 color,
  float4 out gl_Position : POSITION,
  float4 out colorFrag : COLOR,
  float out gl_PointSize : PSIZE
) {
  SET_POINT_SIZE(1.0);
  gl_Position = mul(float4(position, 0.0, 1.0), transform);
  colorFrag = color;
}
)shd";

const char* FRAGMENT_SOURCE_SOLID = R"shd(
float4 main(
  float4 colorFrag : COLOR
) {
  return colorFrag;
}
)shd";


constexpr auto TEXTURED_QUAD_TEXTURE_UNIT_NAMES = std::array{"textureData"};

} // namespace


const char* STANDARD_VERTEX_SOURCE = R"shd(
uniform float4x4 transform;

void main(
  float2 position,
  float2 texCoord,
  float4 out gl_Position : POSITION,
  float2 out texCoordFrag : TEXCOORD0
) {
  gl_Position = mul(float4(position, 0.0, 1.0), transform);
  texCoordFrag = float2(texCoord.x, 1.0 - texCoord.y);
}
)shd";
#else
const char* FRAGMENT_SOURCE_SIMPLE = R"shd(
DEFAULT_PRECISION_DECLARATION
OUTPUT_COLOR_DECLARATION

IN HIGHP vec2 texCoordFrag;

uniform sampler2D textureData;

void main() {
  OUTPUT_COLOR = TEXTURE_LOOKUP(textureData, texCoordFrag);
}
)shd";


const char* FRAGMENT_SOURCE = R"shd(
DEFAULT_PRECISION_DECLARATION
OUTPUT_COLOR_DECLARATION

IN HIGHP vec2 texCoordFrag;

uniform sampler2D textureData;
uniform vec4 overlayColor;

uniform vec4 colorModulation;
uniform bool enableRepeat;

void main() {
  HIGHP vec2 texCoords = texCoordFrag;
  if (enableRepeat) {
    texCoords.x = fract(texCoords.x);
    texCoords.y = fract(texCoords.y);
  }

  vec4 baseColor = TEXTURE_LOOKUP(textureData, texCoords);
  vec4 modulated = baseColor * colorModulation;
  float targetAlpha = modulated.a;

  OUTPUT_COLOR =
    vec4(mix(modulated.rgb, overlayColor.rgb, overlayColor.a), targetAlpha);
}
)shd";

const char* VERTEX_SOURCE_SOLID = R"shd(
ATTRIBUTE vec2 position;
ATTRIBUTE vec4 color;

OUT vec4 colorFrag;

uniform mat4 transform;

void main() {
  SET_POINT_SIZE(1.0);
  gl_Position = transform * vec4(position, 0.0, 1.0);
  colorFrag = color;
}
)shd";

const char* FRAGMENT_SOURCE_SOLID = R"shd(
DEFAULT_PRECISION_DECLARATION
OUTPUT_COLOR_DECLARATION

IN vec4 colorFrag;

void main() {
  OUTPUT_COLOR = colorFrag;
}
)shd";


constexpr auto TEXTURED_QUAD_TEXTURE_UNIT_NAMES = std::array{"textureData"};

} // namespace


const char* STANDARD_VERTEX_SOURCE = R"shd(
ATTRIBUTE HIGHP vec2 position;
ATTRIBUTE HIGHP vec2 texCoord;

OUT HIGHP vec2 texCoordFrag;

uniform mat4 transform;

void main() {
  gl_Position = transform * vec4(position, 0.0, 1.0);
  texCoordFrag = vec2(texCoord.x, 1.0 - texCoord.y);
}
)shd";
#endif

const ShaderSpec TEXTURED_QUAD_SHADER{
  VertexLayout::PositionAndTexCoords,
  TEXTURED_QUAD_TEXTURE_UNIT_NAMES,
  STANDARD_VERTEX_SOURCE,
  FRAGMENT_SOURCE};


const ShaderSpec SIMPLE_TEXTURED_QUAD_SHADER{
  VertexLayout::PositionAndTexCoords,
  TEXTURED_QUAD_TEXTURE_UNIT_NAMES,
  STANDARD_VERTEX_SOURCE,
  FRAGMENT_SOURCE_SIMPLE};


const ShaderSpec SOLID_COLOR_SHADER{
  VertexLayout::PositionAndColor,
  {},
  VERTEX_SOURCE_SOLID,
  FRAGMENT_SOURCE_SOLID};

} // namespace rigel::renderer
