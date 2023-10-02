#shader vertex
#version 440 core

layout(location = 0) in vec3 VertexPosition;
layout(location = 1) in vec2 UVPosition;
layout(location = 2) in vec3 Normal;
layout(location = 3) in vec3 Tangent;
layout(location = 4) in vec3 Bitangent;

uniform mat4 u_Model;
uniform mat4 u_View;
uniform mat4 u_Projection;

out vec4 FragPos;
out mat3 TBN;
out vec2 UV;
out mat4 Inv_Proj;
out mat4 Inv_View;
out mat4 view;
out vec3 o_Tangent;

void main()
{
    //mat3 normalMatrix = transpose(inverse(mat3(u_Model)));
    //vec3 T = normalize(normalMatrix * Tangent);
    //vec3 N = normalize(normalMatrix * Normal);
    //T = normalize(T - dot(T, N) * N);
    //vec3 B = cross(N, T);

    vec3 T = normalize((u_Model * vec4(Tangent, 0.0f)).xyz);
    vec3 N = normalize((u_Model * vec4(Normal, 0.0f)).xyz);
    vec3 B = normalize((u_Model * vec4(Bitangent, 0.0f)).xyz);
    TBN = mat3(T, B, N);

    UV = UVPosition;
    Inv_Proj = u_Projection;
    Inv_View = u_View;
    o_Tangent = Tangent;

    gl_Position = u_Projection * u_View * u_Model * vec4(VertexPosition, 1.0f);
    FragPos = gl_Position;
}

#shader fragment
#version 440 core

layout(location = 0) out vec4 gAlbedo;
layout(location = 1) out vec4 gNormal;
layout(location = 2) out vec4 gMaterial;
layout(location = 3) out int gEntityID;
layout(location = 4) out float gEmissive;
layout(location = 5) out vec4 gTexelOffset;

in vec4 FragPos;
in vec2 UV;
in mat3 TBN;
in mat4 Inv_Proj;
in mat4 Inv_View;


// Material
uniform sampler2D m_Albedo;
uniform sampler2D m_Metalness;
uniform sampler2D m_Roughness;
uniform sampler2D m_AO;
uniform sampler2D m_Normal;
uniform sampler2D m_Displacement;
uniform int u_EntityID;

layout(std140, binding = 32) uniform u_MaterialUniform
{
    int u_HasAlbedo;
    vec3 m_AlbedoColor;     //  16 byte
    int u_HasMetalness;     //  32 byte
    float u_MetalnessValue; //  36 byte
    int u_HasRoughness;     //  40 byte
    float u_RoughnessValue; //  44 byte
    int u_HasAO;            //  48 byte
    float u_AOValue;        //  52 byte
    int u_HasNormal;        //  56 byte
    int u_HasDisplacement;  //  60 byte
    int u_Unlit;
    float u_Emissive;
};

vec3 TextureToWorld(vec2 texCoord, mat4 viewMatrix, mat4 invProjectionMatrix)
{
    // Combine texture coordinate with depth to obtain clip space position
    vec4 clipCoord = vec4(texCoord * 2.0 - 1.0, 0.0, 1.0);

    // Transform clip space position to view space
    vec4 viewCoord = invProjectionMatrix * clipCoord;
    viewCoord /= viewCoord.w;

    // Transform view space position to world space
    vec4 worldCoord = viewMatrix * viewCoord;

    return worldCoord.xyz;
}

void main()
{
    // Normal
    vec3 normal = vec3(0.5, 0.5, 1.0);
    if (u_HasNormal == 1)
    {
        normal = texture(m_Normal, UV).rgb;
    }
    normal = normal * 2.0 - 1.0;
    normal = TBN * normalize(normal);
    gNormal = vec4(normal / 2.0 + 0.5, 1.0f);

    // Albedo
    gAlbedo = vec4(m_AlbedoColor, 1.0f);
    if (u_HasAlbedo == 1)
    {
        vec4 albedoSample = texture(m_Albedo, UV);
        gAlbedo.rgb = albedoSample.rgb * m_AlbedoColor.rgb;
        gAlbedo.a = albedoSample.a;

        if (albedoSample.a < 0.1f)
        {
            discard;
        }
    }

    // Material
    float finalMetalness = u_MetalnessValue;
    if (u_HasMetalness == 1)
        finalMetalness = texture(m_Metalness, UV).r;

    float finalAO = u_AOValue;
    if (u_HasAO == 1)
        finalAO = texture(m_AO, UV).r;

    float finalRoughness = u_RoughnessValue;
    if (u_HasRoughness == 1)
        finalRoughness = texture(m_Roughness, UV).r;

    gMaterial = vec4(0, 0, 0, u_Unlit);
    gMaterial.r = finalMetalness;
    gMaterial.g = finalAO;
    gMaterial.b = finalRoughness;

    gEntityID = int(u_EntityID);

    gEmissive = u_Emissive;

    vec3 right = vec3(1.0, 0, 0);
    right = TBN * normalize(right);
    vec3 up = TBN * normalize(vec3(0, 1, 0));

    vec2 textureSize2d = textureSize(m_Albedo, 0);
    vec2 pixelPos = UV * textureSize2d;
    vec2 centerOfPixel = (vec2(0.5) + floor(pixelPos));
    vec2 uvCenter = centerOfPixel;

    vec3 uvDiff = vec3(uvCenter.x - pixelPos.x, uvCenter.y - pixelPos.y, 0.0f) / vec3(textureSize2d.x, textureSize2d.y, 1.0f);

    uvDiff = uvDiff.x * right;
    uvDiff = uvDiff.y * up;
    //uvDiff * right;
    uvDiff = uvDiff * 100.0f;
    vec2 offset = (centerOfPixel - vec2(FragPos.x, FragPos.y)) * 10.0f;

    vec3 finalTransformed =  normalize(uvDiff);
    vec4 conversion = Inv_Proj * Inv_View * vec4(finalTransformed, 1.0f);
    gTexelOffset = vec4(conversion.x / 2.0 + 0.5f, conversion.y / 2.0 + 0.5f, 0, 1);
}