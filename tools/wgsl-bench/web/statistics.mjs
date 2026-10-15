export function statistics(samples) {
    if (samples.length === 0 || samples.some(value => !Number.isFinite(value) || value < 0)) {
        throw new Error("Expected finite nonnegative timing samples");
    }
    const ordered = [...samples].sort((left, right) => left - right);
    const middle = Math.floor(ordered.length / 2);
    return {
        median_us: ordered.length % 2 ? ordered[middle] : (ordered[middle - 1] + ordered[middle]) / 2,
        min_us: ordered[0],
        p95_us: ordered[Math.ceil(ordered.length * 0.95) - 1],
        samples_us: [...samples],
    };
}

export function randomGenerator(seed) {
    let state = seed >>> 0;
    return () => {
        state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
        return state / 4294967296;
    };
}

export function pairedRatio(handwritten, bwsl) {
    if (handwritten.length === 0 || handwritten.length !== bwsl.length ||
        handwritten.some(value => value <= 0)) {
        return null;
    }
    const ratios = bwsl.map((value, index) => value / handwritten[index]);
    const random = randomGenerator(113);
    const bootstraps = [];
    for (let sample = 0; sample < 1000; ++sample) {
        const resampled = ratios.map(() => ratios[Math.floor(random() * ratios.length)]);
        bootstraps.push(statistics(resampled).median_us);
    }
    bootstraps.sort((left, right) => left - right);
    return {
        median: statistics(ratios).median_us,
        bootstrap_95_interval: [bootstraps[24], bootstraps[974]],
        pairs: ratios,
        scope: "Paired candidate/reference durations; interval excludes systematic timer quantization and thermal effects",
    };
}
