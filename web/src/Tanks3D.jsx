import { useEffect, useRef, useState } from 'react';
import { useRoxal, useRoxalStore } from './roxal-react.js';
import { TankControls } from './TanksPanel.jsx';

// The Babylon view of tanks.rox -- the SAME store that drives the 2D panel,
// with tanks.rox itself unchanged. React owns the page; Babylon is an
// imperative island inside this one component. The load-bearing decision is
// that scene updates NEVER pass through React: the effect below subscribes to
// the store directly and applies the latest snapshot inside Babylon's
// per-frame observable. Roxal ticks at 20 Hz, Babylon renders at ~60 fps, and
// a light exponential lerp bridges the two so the water moves smoothly.
//
// Babylon arrives via dynamic import, so Vite splits it into its own chunk
// and the IDE pays nothing for it until the 3D view first opens.

const BG = '#14161a';

async function buildScene(canvas, store, debug) {
    const [
        { Engine }, { Scene }, { ArcRotateCamera }, { HemisphericLight },
        { PointLight }, { Vector3 }, { Color3, Color4 }, { CreateCylinder },
        { CreateGround }, { StandardMaterial }, { GlowLayer },
        { ParticleSystem }, { DynamicTexture },
    ] = await Promise.all([
        import('@babylonjs/core/Engines/engine.js'),
        import('@babylonjs/core/scene.js'),
        import('@babylonjs/core/Cameras/arcRotateCamera.js'),
        import('@babylonjs/core/Lights/hemisphericLight.js'),
        import('@babylonjs/core/Lights/pointLight.js'),
        import('@babylonjs/core/Maths/math.vector.js'),
        import('@babylonjs/core/Maths/math.color.js'),
        import('@babylonjs/core/Meshes/Builders/cylinderBuilder.js'),
        import('@babylonjs/core/Meshes/Builders/groundBuilder.js'),
        import('@babylonjs/core/Materials/standardMaterial.js'),
        import('@babylonjs/core/Layers/glowLayer.js'),
        import('@babylonjs/core/Particles/particleSystem.js'),
        import('@babylonjs/core/Materials/Textures/dynamicTexture.js'),
    ]);

    const engine = new Engine(canvas, true, { alpha: false });
    const scene = new Scene(engine);
    scene.clearColor = Color4.FromHexString(BG + 'ff');

    // Camera: a gentle orbit the user can grab, clamped so the cascade can
    // never wander out of frame (the first draft let it -- a surge screenshot
    // showed one cropped tank and nothing else).
    const camera = new ArcRotateCamera('cam', -Math.PI / 2.2, 1.25, 19,
                                       new Vector3(0, 4.0, 0), scene);
    camera.attachControl(canvas, true);
    camera.lowerRadiusLimit = 12;
    camera.upperRadiusLimit = 26;
    camera.lowerBetaLimit = 0.95;
    camera.upperBetaLimit = 1.45;
    camera.wheelDeltaPercentage = 0.01;
    camera.useAutoRotationBehavior = true;
    camera.autoRotationBehavior.idleRotationSpeed = 0.07;
    camera.autoRotationBehavior.idleRotationWaitTime = 5000;

    new HemisphericLight('hemi', new Vector3(0, 1, 0), scene).intensity = 0.55;
    const key = new PointLight('key', new Vector3(-6, 9, -6), scene);
    key.intensity = 0.8;
    key.diffuse = Color3.FromHexString('#7dd3fc');

    const glow = new GlowLayer('glow', scene);
    glow.intensity = 0.55;

    // Floor: a dark disc so the tanks sit somewhere rather than floating.
    const ground = CreateGround('ground', { width: 26, height: 26 }, scene);
    const groundMat = new StandardMaterial('groundMat', scene);
    groundMat.diffuseColor = Color3.FromHexString('#181c24');
    groundMat.specularColor = Color3.FromHexString('#0b3a4d');
    ground.material = groundMat;

    // Materials -- the same palette as the 2D panel: cyan water, glass walls,
    // amber alarm.
    const glass = new StandardMaterial('glass', scene);
    glass.diffuseColor = Color3.FromHexString('#7dd3fc');
    glass.alpha = 0.14;
    glass.specularPower = 96;
    glass.backFaceCulling = false;

    const glassAlarm = glass.clone('glassAlarm');
    glassAlarm.diffuseColor = Color3.FromHexString('#fbbf24');
    glassAlarm.emissiveColor = Color3.FromHexString('#4d3a08');
    glassAlarm.alpha = 0.2;

    const waterMat = new StandardMaterial('water', scene);
    waterMat.diffuseColor = Color3.FromHexString('#0ea5e9');
    waterMat.emissiveColor = Color3.FromHexString('#0b4b6e');
    waterMat.specularColor = Color3.FromHexString('#bae6fd');
    waterMat.alpha = 0.85;

    // One tank = glass shell + water cylinder scaled by the level signal, on a
    // dark pedestal. Tank 1 is ELEVATED so its bottom outlet sits above tank
    // 2's rim -- water runs downhill, and the first draft got this wrong
    // (tank 2's rim was above tank 1's outlet, an impossible cascade).
    const TANK_H = 4, TANK_R = 1.5;
    const pedestalMat = new StandardMaterial('pedestal', scene);
    pedestalMat.diffuseColor = Color3.FromHexString('#242a35');
    pedestalMat.specularColor = Color3.FromHexString('#0b3a4d');
    const makeTank = (name, x, baseY) => {
        const shell = CreateCylinder(name + '-shell',
            { height: TANK_H, diameter: TANK_R * 2, tessellation: 48 }, scene);
        shell.position.set(x, baseY + TANK_H / 2, 0);
        shell.material = glass;
        const water = CreateCylinder(name + '-water',
            { height: 1, diameter: TANK_R * 2 - 0.16, tessellation: 48 }, scene);
        water.material = waterMat;
        water.position.set(x, baseY, 0);
        if (baseY > 0.05) {
            const ped = CreateCylinder(name + '-pedestal',
                { height: baseY, diameter: 1.0, tessellation: 24 }, scene);
            ped.position.set(x, baseY / 2, 0);
            ped.material = pedestalMat;
        }
        return { shell, water, x, baseY };
    };
    const tank1 = makeTank('t1', -2.2, 4.6);
    const tank2 = makeTank('t2', 2.0, 0.35);

    // A soft round sprite, generated rather than shipped -- no asset, no CORS.
    const tex = new DynamicTexture('drop', { width: 64, height: 64 }, scene, false);
    {
        const ctx = tex.getContext();
        const g = ctx.createRadialGradient(32, 32, 2, 32, 32, 30);
        g.addColorStop(0, 'rgba(214, 244, 255, 1)');
        g.addColorStop(0.5, 'rgba(125, 211, 252, .55)');
        g.addColorStop(1, 'rgba(125, 211, 252, 0)');
        ctx.fillStyle = g;
        ctx.fillRect(0, 0, 64, 64);
        tex.update();
    }

    // A falling water stream: particles dropped from `from`, dying near `to`.
    const makeStream = (name, from, to) => {
        const ps = new ParticleSystem(name, 400, scene);
        ps.particleTexture = tex;
        ps.emitter = from.clone();
        ps.minEmitBox = new Vector3(-0.06, 0, -0.06);
        ps.maxEmitBox = new Vector3(0.06, 0, 0.06);
        ps.color1 = new Color4(0.49, 0.83, 0.99, 0.9);
        ps.color2 = new Color4(0.13, 0.65, 0.91, 0.8);
        ps.colorDead = new Color4(0.1, 0.3, 0.45, 0);
        ps.minSize = 0.10; ps.maxSize = 0.22;
        const fall = Math.max(0.5, from.y - to.y);
        ps.minLifeTime = Math.sqrt(fall / 9) * 0.9;
        ps.maxLifeTime = Math.sqrt(fall / 9) * 1.15;
        ps.direction1 = new Vector3(-0.05, -1, -0.05);
        ps.direction2 = new Vector3(0.05, -1, 0.05);
        ps.minEmitPower = 1.2; ps.maxEmitPower = 1.8;
        ps.gravity = new Vector3(0, -9, 0);
        ps.emitRate = 0;
        ps.start();
        return ps;
    };

    // Outlets are short pipe stubs at each tank's base; their streams leave
    // with sideways velocity, so gravity draws the pouring ARC a real spout
    // makes. Tank 1's arc lands in tank 2's open top; tank 2's runs to the
    // drain on the floor.
    const makeSpout = (name, x, y) => {
        const stub = CreateCylinder(name, { height: 0.6, diameter: 0.28, tessellation: 16 }, scene);
        stub.rotation.z = Math.PI / 2;
        stub.position.set(x + 0.3, y, 0);
        stub.material = pedestalMat;
        return new Vector3(x + 0.62, y, 0);
    };
    const streamIn = makeStream('in',
        new Vector3(tank1.x, tank1.baseY + TANK_H + 0.5, 0),
        new Vector3(tank1.x, tank1.baseY + 0.3, 0));
    const tip1 = makeSpout('spout1', tank1.x + TANK_R, tank1.baseY + 0.35);
    const stream12 = makeStream('s12', tip1, new Vector3(tank2.x, tank2.baseY + TANK_H, 0));
    stream12.direction1 = new Vector3(1.1, -0.2, -0.04);
    stream12.direction2 = new Vector3(1.32, 0.0, 0.04);
    stream12.minEmitPower = 2.15; stream12.maxEmitPower = 2.45;
    // Lifetime is set per frame from tank 2's LIVE level (see the render
    // observable): the droplets should die where the water actually is.
    stream12.minLifeTime = 0.7; stream12.maxLifeTime = 0.9;
    const tip2 = makeSpout('spout2', tank2.x + TANK_R, tank2.baseY + 0.35);
    const streamOut = makeStream('out', tip2, new Vector3(tip2.x + 1.6, 0, 0));
    streamOut.direction1 = new Vector3(0.8, -0.3, -0.04);
    streamOut.direction2 = new Vector3(1.1, -0.1, 0.04);
    streamOut.minEmitPower = 1.6; streamOut.maxEmitPower = 2.0;
    // Die at the floor, not beneath it.
    streamOut.minLifeTime = 0.35; streamOut.maxLifeTime = 0.5;

    // Overflow spray at tank 1's rim, gated on the `spilling` signal.
    const spray = new ParticleSystem('spray', 300, scene);
    spray.particleTexture = tex;
    spray.emitter = new Vector3(tank1.x, tank1.baseY + TANK_H, 0);
    spray.minEmitBox = new Vector3(-TANK_R * 0.8, 0, -TANK_R * 0.8);
    spray.maxEmitBox = new Vector3(TANK_R * 0.8, 0, TANK_R * 0.8);
    spray.color1 = new Color4(0.99, 0.88, 0.5, 0.95);
    spray.color2 = new Color4(0.49, 0.83, 0.99, 0.85);
    spray.colorDead = new Color4(0.2, 0.4, 0.55, 0);
    spray.minSize = 0.08; spray.maxSize = 0.2;
    spray.minLifeTime = 0.5; spray.maxLifeTime = 0.9;
    spray.direction1 = new Vector3(-0.7, 1, -0.7);
    spray.direction2 = new Vector3(0.7, 1.6, 0.7);
    spray.minEmitPower = 1.5; spray.maxEmitPower = 3;
    spray.gravity = new Vector3(0, -9, 0);
    spray.emitRate = 0;
    spray.start();

    // --- the data path: store snapshot -> scene, once per rendered frame ----
    let latest = store.getSnapshot();
    const unsubscribe = store.subscribe(snap => { latest = snap; });

    const state = { l1: 0, l2: 0 };
    const EPS = 0.004;
    scene.onBeforeRenderObservable.add(() => {
        const dt = engine.getDeltaTime() / 1000;
        const k = 1 - Math.exp(-dt * 8);           // ~120ms to close the gap
        const cap = latest.capacity ?? 10;
        state.l1 += (((latest.level1 ?? 0) / cap) - state.l1) * k;
        state.l2 += (((latest.level2 ?? 0) / cap) - state.l2) * k;

        for (const [t, frac] of [[tank1, state.l1], [tank2, state.l2]]) {
            const h = Math.max(EPS, frac) * (TANK_H - 0.1);
            t.water.scaling.y = h;                  // unit-height cylinder
            t.water.position.y = t.baseY + h / 2 + 0.02;
            t.water.isVisible = frac > EPS;
        }
        tank1.shell.material = latest.spilling ? glassAlarm : glass;

        // The 1→2 arc dies at tank 2's water surface, wherever the level
        // currently puts it: a fuller tank meets the droplets sooner. The
        // constants are screenshot-calibrated, not ballistic -- too short and
        // the arc vanishes above the surface, too long and its tail drifts
        // past the far wall at floor depth (both were tried and looked wrong).
        const drop = 1 - state.l2;
        stream12.minLifeTime = 0.62 + drop * 0.35;
        stream12.maxLifeTime = stream12.minLifeTime + 0.18;

        // Streams follow the flow signals; ~55 particles per m³/s reads well,
        // and the droplet size grows a little with the rate so a surge pours
        // visibly thicker rather than just denser.
        for (const [ps, rate] of [[streamIn, latest.inflow ?? 0],
                                  [stream12, latest.out1 ?? 0],
                                  [streamOut, latest.out2 ?? 0]]) {
            ps.emitRate = rate * 55;
            ps.minSize = 0.08 + rate * 0.012;
            ps.maxSize = 0.18 + rate * 0.02;
        }
        spray.emitRate = latest.spilling ? 220 : 0;

        // What the SCENE is showing, for the browser tests -- the whole chain
        // signals -> store -> subscription -> render, in numbers.
        debug.ready = true;
        debug.level1 = state.l1 * cap;
        debug.level2 = state.l2 * cap;
    });

    const render = () => scene.render();
    engine.runRenderLoop(render);
    const ro = new ResizeObserver(() => engine.resize());
    ro.observe(canvas);

    return {
        // Pausing stops the RENDER LOOP -- the GPU and the per-frame particle
        // work go to zero -- while the store subscription stays attached, so
        // resuming snaps straight to wherever the (still running) network has
        // taken the plant. Pausing the SCRIPT instead would be the worst of
        // both: a frozen simulation with the renderer still burning.
        pause(p) {
            engine.stopRenderLoop();
            if (!p) engine.runRenderLoop(render);
            debug.paused = p;
        },
        dispose() {
            ro.disconnect();
            unsubscribe();
            engine.stopRenderLoop();
            scene.dispose();
            engine.dispose();
        },
    };
}

export default function Tanks3D({ rox, paused = false }) {
    const t = useRoxal(rox, 'tanks');            // controls re-render from this
    const store = useRoxalStore(rox, 'tanks');
    const canvasRef = useRef(null);
    const [failed, setFailed] = useState(null);
    const controlRef = useRef(null);

    useEffect(() => {
        let gone = false;
        const debug = { ready: false, paused: false, level1: 0, level2: 0 };
        window.__tanks3d = debug;
        buildScene(canvasRef.current, store, debug)
            .then(c => {
                if (gone) { c.dispose(); return; }
                controlRef.current = c;
                if (pausedRef.current) c.pause(true);
            })
            .catch(e => setFailed(String(e.message || e)));
        return () => { gone = true; controlRef.current?.dispose(); controlRef.current = null; delete window.__tanks3d; };
        // Mount once per store generation (the parent keys this component).
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const pausedRef = useRef(paused);
    pausedRef.current = paused;
    useEffect(() => { controlRef.current?.pause(paused); }, [paused]);

    if (t.level1 === undefined)
        return <p className="loading">this app exposes no "tanks" store — watch the output pane</p>;

    return (
        <div className="panel tanks-panel tanks3d">
            {failed
                ? <pre className="error">3D view failed: {failed}</pre>
                : <canvas ref={canvasRef} className="tanks3d-canvas" />}
            <TankControls t={t} store={store} />
        </div>
    );
}
