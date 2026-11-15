// What an effect says about itself (plan M20 step 2). An effect is a Slang file:
//
//   import effect;
//
//   struct Params
//   {
//       [Label("Exposure")] [Range(-5, 5)] [Default(0)] [Unit("stops")] float exposure;
//       uint2 size; // no label: not shown; Lumen fills in "size"
//   };
//   [[vk::push_constant]] ConstantBuffer<Params> p;
//   [[vk::binding(0)]] [format("rgba16f")] RWTexture2D<float4> source; // the picture so far
//   [[vk::binding(1)]] [format("rgba16f")] RWTexture2D<float4> result; // what this effect makes
//
//   [Effect("Exposure", "Light")]
//   [shader("compute")] [numthreads(16, 16, 1)]
//   void main(uint3 id: SV_DispatchThreadID) { ... }
//
// Its compute entries are its passes, in order. Pictures are in linear light, RGBA half floats with
// straight alpha. A binding named neither "source" nor "result" is a texture of the same size the
// effect keeps between its passes (a blur across, then down). All passes share the parameters.

// The effect's name and its group in the Develop panel; on the first pass.
[__AttributeUsage(_AttributeTargets.Function)]
struct EffectAttribute
{
    string title;
    string group;
};

// A parameter's name on its slider. Parameters without one are not shown.
[__AttributeUsage(_AttributeTargets.Var)]
struct LabelAttribute
{
    string text;
};

[__AttributeUsage(_AttributeTargets.Var)]
struct RangeAttribute
{
    float minimum;
    float maximum;
};

[__AttributeUsage(_AttributeTargets.Var)]
struct DefaultAttribute
{
    float value;
};

[__AttributeUsage(_AttributeTargets.Var)]
struct UnitAttribute
{
    string text;
};
