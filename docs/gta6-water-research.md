# GTA6 水面生成技术调研报告

> 调研日期：2026-11-24
> 目的：分析 GTA6（RAGE 9 引擎）水面效果的生成原理，并映射到本项目（`g:\Trae\vulkan`）的可实现子集。
> 说明：Rockstar 未发布 GTA6 水系统的官方技术讲座，本文严格区分【确认事实】（有帧捕获/官方资料支撑）、【社区推断】（技术博客/媒体分析）与【通用技术】（业界成熟方案）。

---

## 一、GTA6 水面效果拆解（观察 → 推断技术）

从官方预告片与媒体分析可观察到的水面特征：

| 观察到的效果 | 推断的技术 |
|---|---|
| 海面有真实的起伏波浪，浪脊尖锐、波谷宽缓 | 波浪形变不再是"贴图动画"，而是**几何位移**（Gerstner/FFT 或更高阶流体解算） |
| 浪传播到浅滩时会减速、卷起、破碎 | **水深场（Bathymetry）交互**：海底摩擦使波基减速、波峰保持前速，产生天然的浅化（shoaling）与破碎 |
| 浪破碎时产生飞溅水花、粒子喷雾 | **拉格朗日粒子解耦**：波峰陡度超阈值时，连续流体面解耦为数千离散 3D 粒子（受空气阻力），产生 spray/splash/wake |
| 船只斜向冲浪会腾空、翻滚、砸落水面 | **静水浮力 + 刚体耦合**：船体下方实时读取局部压力/速度场，连续计算水动升力与阻力 |
| 天空/城市在水面上的反射随视角流动 | 硬件光线追踪（RAGE 9 主打特性）或高质量屏幕空间反射 |

**置信度标注**：以上"Navier-Stokes compute shader 解算 + Bathymetry + 拉格朗日粒子"的完整描述来自技术博客 Reach Technocracy 的深度分析（2026-09），属于【社区推断】——Beebom 等媒体也确认 "Rockstar has not publicly confirmed the full technical details of its water simulation"。但 GTA6 水面从"数学曲面"升级为"实时物理模拟水体"这一大方向，有多来源交叉印证（见 ofzenandcomputing 引用的 2023 年 RAGE 技术报告）。

---

## 二、RAGE 引擎水系统的演进（有据可查的演进线）

| 引擎版本 | 作品 | 水方案 |
|---|---|---|
| RAGE 7 | GTA V (2013) | **数学曲面 + 动画贴图**（无流体模拟） |
| RAGE 8 | RDR2 (2018) | **手工设计的波形**（authored wave shapes），仍无真正流体模拟 |
| RAGE 9 | GTA6 | **实时物理模拟水体**（多来源指向） |

### 2.1 GTA V 水渲染的实际管线【确认事实】

Adrian Courrèges 对 GTA V 的帧捕获分析（Graphics Study）揭示了当时的水渲染管线，即使引擎演进，这套**渲染侧架构**仍高度可参考：

1. **反射 + 折射双通道**：水面同时处理 reflection 与 refraction
2. **深度预处理**：先由对数 Z-buffer 生成半分辨率的**线性深度图**，供水面 shader 采样（软边缘、水体深度吸收）
3. **MRT 多目标输出**：海洋/泳池逐个以 MRT 模式绘制，同时输出 Water Diffuse、Water Opacity 等多张目标图，供后续合成（水体与雾、水下效果的分层混合）

### 2.2 RDR2 的水设置项【确认事实】

从 RDR2 的图形设置描述可确认 RAGE 8 已具备：

- **GPU Compute Shader FFT 波浪模拟**（高画质档）
- **自适应曲面细分（tessellation）**用于水面 displacement
- **屏幕空间折射**（扭曲的深度缓冲采样）
- **平面反射 / 屏幕空间反射**

→ 由此推断 GTA6 的水系统是这条线的直接升级：模拟侧从 FFT 波谱升级到流体解算（推断），渲染侧继续沿用"深度图 + 折射 + 反射 + MRT 合成"的成熟架构（推断）。

---

## 三、核心技术原理详解（通用技术，可直接实现）

### 3.1 Gerstner 波（本轮实现的核心）

Gerstner 波（1802/1804 年提出）是绝大多数游戏海洋渲染的基础。与纯垂直正弦波的区别：水质点做**竖直面内的圆周运动（trochoid 摆线轨道）**，波峰尖锐、波谷宽缓，这正是真实海浪的形状。

对第 i 个波（方向 D̂ 归一化、波长 λ、陡度 Q、振幅 A、初相 φ₀）：

```
k  = 2π / λ                      // 波数
ω  = √(g · k)                    // 色散关系（长波跑得快）
φ  = k · dot(D, P₀.xz) − ω·t + φ₀

P.x += Q·A · D.x · cos(φ)        // 水平位移 → 尖峰
P.y += A · sin(φ)                // 垂直位移
P.z += Q·A · D.z · cos(φ)
```

**关键约束**：Σᵢ Qᵢ·Aᵢ·kᵢ ≤ 1.0（超过则波面"翻卷打结"，出现自交伪影）。
典型取值：平静海面 Q≈0.2–0.4，风暴海面 Q≈0.6–0.9（单波）。实际叠加 4–8 个不同方向（±30°）、不同波长的波：大波定主运动，小波做细节涟漪。

**解析法线**（无需法线贴图、无需邻点差分）：

```
WA = A · k
N.x = −Σ D.x · WA · cos(φ)
N.y =  1 − Σ Q · WA · sin(φ)
N.z = −Σ D.z · WA · cos(φ)
N   = normalize(N)
```

这是 Gerstner 方程偏导数的解析结果，比数值差分更快更准。

### 3.2 Fresnel 反射（Schlick 近似）

水在掠射角近乎镜面、俯视时近乎透明：

```
F(θ) = F₀ + (1 − F₀) · (1 − cosθ)⁵     // F₀ ≈ 0.02（水，n₁=1.0 → n₂=1.333）
color = mix(waterColor, reflectColor, F)
```

- 视线与水平面夹角 ~15° 时 F≈0.8–0.9 → 几乎全反射（看到天空）
- 俯视时 F≈0.02 → 看到水体本身的深色

### 3.3 波峰泡沫

泡沫出现在波面"折叠"最剧烈处。廉价近似：**形变雅可比 ≈ 1 − N.y**（法线越倾斜，波越陡）：

```
foam = smoothstep(0.55, 0.8, 1 − N.y)
color = mix(oceanColor, white, foam)
```

更高级做法（GTA6 级）：把 Jacobian 存入泡沫累积纹理，逐帧衰减（foam accumulation），产生持久的白色泡沫轨迹。本轮用静态近似。

### 3.4 深度吸收（水体颜色）

真实水体对红光吸收最强。游戏常用近似：按视线穿过水体的距离/深度做颜色指数衰减：

```
waterColor = mix(deepColor, shallowColor, exp(−dist · k))
```

GTA V 用半分辨率线性深度图做真深度计算；本项目用世界坐标距离近似（无深度缓冲）。

### 3.5 工业级方案的分级（未来扩展路线）

| 等级 | 技术 | 成本 |
|---|---|---|
| 入门 | 4–8 个 Gerstner 波（顶点着色器）+ 解析法线 | 极低（本轮实现） |
| 进阶 | 波浪参数 UBO 化 + 泡沫累积纹理 + 深度缓冲折射 | 中 |
| 工业 | Compute Shader FFT（Phillips 波谱）+ 曲面细分 LOD（clipmap）+ SSR | 高（RDR2/GTA6 路线） |
| 旗舰 | Navier-Stokes 流体解算 + Bathymetry + 拉格朗日粒子 spray | 极高（GTA6 推断方案） |

---

## 四、本项目实现映射表

| GTA6/RAGE 技术 | 本项目对应实现 | 状态 |
|---|---|---|
| 几何波动 | 6 个 Gerstner 波叠加，顶点着色器位移（192×192 网格） | ✅ 本轮 |
| 法线 | Gerstner 解析法线（偏导推导） | ✅ 本轮 |
| Fresnel 反射 | Schlick 近似，F₀=0.02，反射色用程序化天空渐变近似 | ✅ 本轮 |
| 太阳镜面高光 | Blinn-Phong（sunDir/cameraPos 从 UBO 传入） | ✅ 本轮 |
| 波峰泡沫 | Jacobian 近似（1 − N.y）+ 纹理噪声破碎 | ✅ 本轮 |
| 深度吸收 | 视距指数衰减颜色（近浅远深） | ✅ 本轮 |
| 波峰透光（SSS 近似） | 波峰高度 × 视线-太阳夹角散射 | ✅ 本轮（轻量） |
| 水纹理 | 保留现有程序化纹理，降级为细节法线扰动 + 泡沫噪声源 | ✅ 本轮 |
| 半分辨率线性深度图（GTA V） | 需深度缓冲 | ⏳ 未来 |
| 真实反射（平面/SSR） | 需离屏 RenderTarget | ⏳ 未来 |
| FFT 波谱（RDR2） | 需 Compute Pipeline | ⏳ 未来 |
| 泡沫累积轨迹 | 需持久化纹理 + 逐帧衰减 | ⏳ 未来 |
| 浅滩破碎（Bathymetry） | 需海底地形场 | ⏳ 未来 |
| 粒子 spray/wake | 需粒子系统 | ⏳ 未来 |
| 浮力/船只交互 | 需物理引擎耦合 | ⏳ 未来 |

## 五、参考来源

- [Reach Technocracy — GTA 6 Software Engineering: Navier-Stokes, Ray Tracing & AI](https://reachtechnocracy.com/en/the-software-mega-engineering-of-gta-6-how-navier-stokes-physics-path-tracing-and-ai-redefine-virtual-worlds/)（社区推断：RAGE 9 流体管线）
- [Of Zen and Computing — RAGE Engine in GTA 6: Physics, Water Dynamics & Graphics Upgrades](https://www.ofzenandcomputing.com/rage-engine-gta-6-physics-water-dynamics-graphics-upgrades/)（RAGE 7/8/9 水方案演进表）
- [Beebom — RAGE Engine in GTA 6](https://beebom.com/gta-6-rage-engine-features-upgrades/)（"Rockstar 未官方确认完整技术细节"）
- [Adrian Courrèges — GTA V Graphics Study](https://www.adriancourreges.com/blog/2015/11/02/gta-v-graphics-study/)（GTA V 水渲染管线帧捕获分析）
- [TargetFPS — RDR2 Graphics Settings](https://targetfps.com/games/red-dead-redemption-2/)（RDR2 FFT 波浪/SSR 设置项）
- [NVIDIA GPU Gems Chapter 1 — Effective Water Simulation from Physical Models](https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models)（Gerstner 波与解析法线原始出处）
- [SimLab — Gerstner Waves: ocean mathematics and GLSL](https://mysimulator.uk/content/articles/gerstner-waves.html)（GLSL 实现公式：位移、解析法线、Fresnel、泡沫）
