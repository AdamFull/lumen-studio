// Kernels for tst_gpu: parameters of each kind, and many dispatches on one texture.

struct TestParams
{
    float4 color;
    float amount;
    int shift;
    uint2 size;
};
[[vk::push_constant]] ConstantBuffer<TestParams> p;

[[vk::binding(0)]] [format("rgba16f")] RWTexture2D<float4> target;

// The colour, plus `shift` on every channel.
[shader("compute")]
[numthreads(8, 8, 1)]
void fill(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    target[id.xy] = p.color + float(p.shift);
}

// Adds `amount` to red, green and blue in place: each dispatch depends on the one before.
[shader("compute")]
[numthreads(8, 8, 1)]
void add(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    target[id.xy] = target[id.xy] + float4(p.amount, p.amount, p.amount, 0.0);
}
