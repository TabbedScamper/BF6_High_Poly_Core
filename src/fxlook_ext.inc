/* ONE EMITTER LAYER'S LOOK, resolved from its parameter table.
 *
 * A layer's `params` is the emitter template's defaults with the layer's own
 * overrides applied on top, addressed by `pid` - the djb2-xor of the
 * parameter's name. Naming those pids is the whole job here, and it belongs in
 * the core for the same reason the material categories do: the moment each
 * editor carries its own copy of the table they can disagree about what an
 * effect looks like, and the disagreement shows up as "the FX look wrong in
 * Godot" rather than as a mismatched constant.
 *
 * The names and ids come from data/fx_property_ids_v2.tsv in the research repo
 * (2,525 named parameters with types), not from guessing at hashes. The usage
 * counts beside each are the shipped override counts, which is what says these
 * are the ones worth resolving: RotationOverLife is patched 8,814 times.
 *
 * NOTHING IS INVENTED. A field the layer does not author keeps its zero and its
 * `has` bit stays clear, so a consumer can tell an authored 0 from an absent
 * value and decide for itself rather than inheriting a default chosen here. */

/* @gen:fxlook_pids */
static const uint32_t kFx_base_size = 0xBD355AD5u;            /* BaseSize 22,704 */
static const uint32_t kFx_base_size_bias = 0xBB40784Cu;       /* BaseSizeBias 3,034 */
static const uint32_t kFx_size_x = 0x0DCF4958u;               /* SizeX 5,416 */
static const uint32_t kFx_size_y = 0x0DCF4959u;               /* SizeY 4,949 */
static const uint32_t kFx_size_z = 0x0DCF495Au;               /* SizeZ 5,340 */
static const uint32_t kFx_spawn_size = 0x8A1C76DBu;           /* SpawnSize 6,424 */
static const uint32_t kFx_size_over_life = 0x0AFB1F08u;       /* SizeOverLife 118 */
static const uint32_t kFx_size_curve = 0x8B256B37u;           /* SizeCurve 5,898 */
static const uint32_t kFx_size_y_mult = 0x8C70A459u;          /* SizeYMult 4,872 */
static const uint32_t kFx_scale = 0x0DC8309Du;                /* Scale 2,453 */
static const uint32_t kFx_scale_x_mult = 0x539877A5u;         /* ScaleXMult 916 */
static const uint32_t kFx_scale_z_mult = 0x537688A7u;         /* ScaleZMult 915 */
static const uint32_t kFx_pivot_y = 0xC9846F88u;              /* PivotY 5,131 */
static const uint32_t kFx_opacity = 0x39F20FDCu;              /* Opacity 24,145 */
static const uint32_t kFx_opacity_mult = 0x01C5CADCu;         /* OpacityMult */
static const uint32_t kFx_opacity_over_life = 0x3D1607D4u;    /* OpacityOverLife 22,358 */
static const uint32_t kFx_color = 0x0CA8C5F8u;                /* Color */
static const uint32_t kFx_color_mult = 0x1EE568B8u;           /* ColorMult */
static const uint32_t kFx_color0 = 0xA1C184C8u;               /* Color0 16,276 */
static const uint32_t kFx_color1 = 0xA1C184C9u;               /* Color1 12,742 */
static const uint32_t kFx_random_color_min = 0xE5C35109u;     /* RandomColorMin 13,683 */
static const uint32_t kFx_random_color_max = 0xE5C35217u;     /* RandomColorMax 13,883 */
static const uint32_t kFx_intensity = 0xE4AABCEAu;            /* Intensity 1,435 */
static const uint32_t kFx_temperature = 0x84477F09u;          /* Temperature 1,485 */
static const uint32_t kFx_ramp = 0x7C896E0Bu;                 /* Ramp 2,596 */
static const uint32_t kFx_alpha_cull = 0x1487F4B0u;           /* AlphaCullThreshold */
static const uint32_t kFx_rotation_speed = 0x2FD2E956u;       /* RotationSpeed 27,938 */
static const uint32_t kFx_rotation_speed_bias = 0x47553DCFu;  /* RotationSpeedBias 4,167 */
static const uint32_t kFx_rotation_over_life = 0x0C8950D9u;   /* RotationOverLife 8,814 */
static const uint32_t kFx_spawn_rotation_min = 0x80860520u;   /* SpawnRotationMin 1,331 */
static const uint32_t kFx_spawn_rotation_max = 0x8086043Eu;   /* SpawnRotationMax 1,652 */
static const uint32_t kFx_spawn_rotation_speed = 0xBB9A5E6Du; /* SpawnRotationSpeed 6,851 */
static const uint32_t kFx_spawn_rotation_speed_mult = 0xA56991ADu;/* SpawnRotationSpeedMult 1,258 */
static const uint32_t kFx_spawn_rotation_speed_min_mult = 0x4C7F1E67u;/* SpawnRotationSpeedMinMult 1,022 */
static const uint32_t kFx_spawn_speed = 0xCDB76C39u;          /* SpawnSpeed 28,486 */
static const uint32_t kFx_spawn_speed_mult = 0x77706079u;     /* SpawnSpeedMult 6,091 */
static const uint32_t kFx_spawn_speed_min_mult = 0xF5BFFA33u; /* SpawnSpeedMinMult 15,502 */
static const uint32_t kFx_spawn_speed_bias = 0x77711EE0u;     /* SpawnSpeedBias 1,170 */
static const uint32_t kFx_spawn_speed_curve = 0x65A739AEu;    /* SpawnSpeedCurve 2,884 */
static const uint32_t kFx_speed_mult = 0x3DAD1982u;           /* SpeedMult 3,903 */
static const uint32_t kFx_drag = 0x7C7FD695u;                 /* Drag 30,351 */
static const uint32_t kFx_drag_min_mult = 0x5A7B475Fu;        /* DragMinMult 21,326 */
static const uint32_t kFx_drag_over_life = 0x2339F49Du;       /* DragOverLife 11,602 */
static const uint32_t kFx_gravity = 0xC46720E3u;              /* Gravity 12,616 */
static const uint32_t kFx_buoyancy = 0x4E4BFB91u;             /* Buoyancy 15,173 */
static const uint32_t kFx_buoyancy_over_life = 0x384AF999u;   /* BuoyancyOverLife 7,341 */
static const uint32_t kFx_wind_strength = 0xE0A01AD4u;        /* WindStrength 28,123 */
static const uint32_t kFx_random_force = 0x441BEE03u;         /* RandomForce 18,590 */
static const uint32_t kFx_random_force_over_life = 0x3858A30Bu;/* RandomForceOverLife 2,346 */
static const uint32_t kFx_local_force = 0xEFC8A035u;          /* LocalForce 4,457 */
static const uint32_t kFx_local_force_over_life = 0x12079D3Du;/* LocalForceOverLife 3,167 */
static const uint32_t kFx_direction_from_origin = 0x27319714u;/* DirectionFromEmitterOrigin 3,468 */
static const uint32_t kFx_restitution = 0x8906E021u;          /* Restitution 7,340 */
static const uint32_t kFx_restitution_variance = 0xB663BD04u; /* RestitutionVariance 1,097 */
static const uint32_t kFx_friction = 0x12405B67u;             /* Friction 6,890 */
static const uint32_t kFx_friction_variance = 0x773DD7C2u;    /* FrictionVariance 1,012 */
static const uint32_t kFx_spawn_position = 0xFECEFDE7u;       /* SpawnPosition 5,893 */
static const uint32_t kFx_spawn_scale = 0xCDAE4A26u;          /* SpawnScale 2,013 */
static const uint32_t kFx_inner_radius = 0xC6BBC2C3u;         /* InnerRadius 1,598 */
static const uint32_t kFx_outer_radius = 0xCD102164u;         /* OuterRadius 5,836 */
static const uint32_t kFx_start_zenith_angle = 0x3107BE60u;   /* StartZenithAngle 1,021 */
static const uint32_t kFx_end_zenith_angle = 0xDC56DBCFu;     /* EndZenithAngle 1,316 */
static const uint32_t kFx_sun_light_scale = 0xF36E7E8Bu;      /* SunLightScale 5,873 */
static const uint32_t kFx_local_light_scale = 0x209877EEu;    /* LocalLightScale 4,759 */
static const uint32_t kFx_ambient_light_scale = 0x1558A27Bu;  /* AmbientLightScale 1,876 */
static const uint32_t kFx_received_shadow_scale = 0xEB0563F4u;/* ReceivedShadowScale */
static const uint32_t kFx_cloud_shadow_scale = 0xFF6899EAu;   /* CloudShadowScale */
static const uint32_t kFx_gnomon_backlight = 0xCC64D81Au;     /* GnomonBacklight 17,083 */
static const uint32_t kFx_vertex_backlight = 0xC0246618u;     /* VertexBacklight 16,856 */
static const uint32_t kFx_backlight_contrast = 0x8454003Au;   /* BacklightPixelContrast 17,345 */
static const uint32_t kFx_shadow_radius_mult = 0x973336DBu;   /* ShadowRadiusMult 1,021 */
static const uint32_t kFx_light_mult_type = 0x1F0B9D63u;      /* LightMultType */
static const uint32_t kFx_gnomon_rig_index = 0x27F4CED3u;     /* GnomonLightRigIndex */
static const uint32_t kFx_camera_bias = 0xBFF3F865u;          /* CameraBias 12,256 */
static const uint32_t kFx_inv_z_fade = 0x141FF6C3u;           /* InvZFadeMultiplier 8,714 */
static const uint32_t kFx_disable_frame_blend = 0x45056B2Du;  /* DisableFrameBlend */
static const uint32_t kFx_use_right_tile = 0xCB4FC2D2u;       /* UseRightTile */
static const uint32_t kFx_mirror = 0x9CFC66BCu;               /* Mirror 6,162 */
static const uint32_t kFx_flip_u_probability = 0x694CE72Eu;   /* FlipUProbability 5,137 */
static const uint32_t kFx_flip_v_probability = 0x9FD600CDu;   /* FlipVProbability 14,561 */
static const uint32_t kFx_based_on_lifetime = 0xA05DCC66u;    /* BasedOnLifetime 6,130 */
static const uint32_t kFx_local_space = 0xF11A114Cu;          /* LocalSpace */
/* @end:fxlook_pids */

/* THE CONTROL. A pid one bit from a real one must not resolve, or the table
 * lookup is matching on something other than the id it was given. */
static const uint32_t kFxFakePid = 0xBD355AD4u;

static const bf6_fx_param* fxlook_find(const bf6_fx_layer* l, uint32_t pid)
{
    if (!l || !l->params) return nullptr;
    for (int i = 0; i < l->param_count; ++i)
        if (l->params[i].pid == pid) return &l->params[i];
    return nullptr;
}

int bf6_fx_layer_look(const bf6_fx_layer* layer, bf6_fx_look* out)
{
    if (!out) return 0;
    *out = bf6_fx_look{};
    if (!layer) return 0;
    int authored = 0;
    /* Two words now: the look passed 64 fields. Indexing by hand is how the
     * second word gets forgotten, so every reader goes through
     * bf6_fx_look_authored. */
    auto mark = [&](int bit) {
        out->has[(unsigned)bit >> 6] |= (1ull << ((unsigned)bit & 63));
        authored++;
    };

    auto f1 = [&](uint32_t pid, float* dst, int bit) {
        if (const bf6_fx_param* p = fxlook_find(layer, pid)) { *dst = p->v[0]; mark(bit); }
    };
    auto i1 = [&](uint32_t pid, int* dst, int bit) {
        if (const bf6_fx_param* p = fxlook_find(layer, pid)) { *dst = p->ivalue; mark(bit); }
    };
    auto f4 = [&](uint32_t pid, float* dst, int bit) {
        if (const bf6_fx_param* p = fxlook_find(layer, pid)) {
            for (int k = 0; k < 4; ++k) dst[k] = p->v[k];
            mark(bit);
        }
    };
    auto f3 = [&](uint32_t pid, float* dst, int bit) {
        if (const bf6_fx_param* p = fxlook_find(layer, pid)) {
            for (int k = 0; k < 3; ++k) dst[k] = p->v[k];
            mark(bit);
        }
    };
    /* Vec2 read with the Vec3 helper walks one float past the value. The
     * arity comes from the property table's own type column, so this cannot
     * drift from what the game says the parameter is. */
    auto f2 = [&](uint32_t pid, float* dst, int bit) {
        if (const bf6_fx_param* p = fxlook_find(layer, pid)) {
            for (int k = 0; k < 2; ++k) dst[k] = p->v[k];
            mark(bit);
        }
    };

/* @gen:fxlook_resolve */
    f1(kFx_base_size, &out->base_size, BF6_FXLOOK_BASE_SIZE);
    f1(kFx_base_size_bias, &out->base_size_bias, BF6_FXLOOK_BASE_SIZE_BIAS);
    f1(kFx_size_x, &out->size_x, BF6_FXLOOK_SIZE_X);
    f1(kFx_size_y, &out->size_y, BF6_FXLOOK_SIZE_Y);
    f1(kFx_size_z, &out->size_z, BF6_FXLOOK_SIZE_Z);
    f2(kFx_spawn_size, out->spawn_size, BF6_FXLOOK_SPAWN_SIZE);
    f4(kFx_size_over_life, out->size_over_life, BF6_FXLOOK_SIZE_OVER_LIFE);
    f4(kFx_size_curve, out->size_curve, BF6_FXLOOK_SIZE_CURVE);
    f1(kFx_size_y_mult, &out->size_y_mult, BF6_FXLOOK_SIZE_Y_MULT);
    f3(kFx_scale, out->scale, BF6_FXLOOK_SCALE);
    f1(kFx_scale_x_mult, &out->scale_x_mult, BF6_FXLOOK_SCALE_X_MULT);
    f1(kFx_scale_z_mult, &out->scale_z_mult, BF6_FXLOOK_SCALE_Z_MULT);
    f1(kFx_pivot_y, &out->pivot_y, BF6_FXLOOK_PIVOT_Y);
    f1(kFx_opacity, &out->opacity, BF6_FXLOOK_OPACITY);
    f1(kFx_opacity_mult, &out->opacity_mult, BF6_FXLOOK_OPACITY_MULT);
    f4(kFx_opacity_over_life, out->opacity_over_life, BF6_FXLOOK_OPACITY_OVER_LIFE);
    f3(kFx_color, out->color, BF6_FXLOOK_COLOR);
    f3(kFx_color_mult, out->color_mult, BF6_FXLOOK_COLOR_MULT);
    f3(kFx_color0, out->color0, BF6_FXLOOK_COLOR0);
    f3(kFx_color1, out->color1, BF6_FXLOOK_COLOR1);
    f3(kFx_random_color_min, out->random_color_min, BF6_FXLOOK_RANDOM_COLOR_MIN);
    f3(kFx_random_color_max, out->random_color_max, BF6_FXLOOK_RANDOM_COLOR_MAX);
    f1(kFx_intensity, &out->intensity, BF6_FXLOOK_INTENSITY);
    f1(kFx_temperature, &out->temperature, BF6_FXLOOK_TEMPERATURE);
    i1(kFx_ramp, &out->ramp, BF6_FXLOOK_RAMP);
    f1(kFx_alpha_cull, &out->alpha_cull, BF6_FXLOOK_ALPHA_CULL);
    f1(kFx_rotation_speed, &out->rotation_speed, BF6_FXLOOK_ROTATION_SPEED);
    f1(kFx_rotation_speed_bias, &out->rotation_speed_bias, BF6_FXLOOK_ROTATION_SPEED_BIAS);
    f4(kFx_rotation_over_life, out->rotation_over_life, BF6_FXLOOK_ROTATION_OVER_LIFE);
    f3(kFx_spawn_rotation_min, out->spawn_rotation_min, BF6_FXLOOK_SPAWN_ROTATION_MIN);
    f3(kFx_spawn_rotation_max, out->spawn_rotation_max, BF6_FXLOOK_SPAWN_ROTATION_MAX);
    f1(kFx_spawn_rotation_speed, &out->spawn_rotation_speed, BF6_FXLOOK_SPAWN_ROTATION_SPEED);
    f2(kFx_spawn_rotation_speed_mult, out->spawn_rotation_speed_mult, BF6_FXLOOK_SPAWN_ROTATION_SPEED_MULT);
    f1(kFx_spawn_rotation_speed_min_mult, &out->spawn_rotation_speed_min_mult, BF6_FXLOOK_SPAWN_ROTATION_SPEED_MIN_MULT);
    f1(kFx_spawn_speed, &out->spawn_speed, BF6_FXLOOK_SPAWN_SPEED);
    f2(kFx_spawn_speed_mult, out->spawn_speed_mult, BF6_FXLOOK_SPAWN_SPEED_MULT);
    f1(kFx_spawn_speed_min_mult, &out->spawn_speed_min_mult, BF6_FXLOOK_SPAWN_SPEED_MIN_MULT);
    f1(kFx_spawn_speed_bias, &out->spawn_speed_bias, BF6_FXLOOK_SPAWN_SPEED_BIAS);
    f4(kFx_spawn_speed_curve, out->spawn_speed_curve, BF6_FXLOOK_SPAWN_SPEED_CURVE);
    f1(kFx_speed_mult, &out->speed_mult, BF6_FXLOOK_SPEED_MULT);
    f1(kFx_drag, &out->drag, BF6_FXLOOK_DRAG);
    f1(kFx_drag_min_mult, &out->drag_min_mult, BF6_FXLOOK_DRAG_MIN_MULT);
    f4(kFx_drag_over_life, out->drag_over_life, BF6_FXLOOK_DRAG_OVER_LIFE);
    f1(kFx_gravity, &out->gravity, BF6_FXLOOK_GRAVITY);
    f1(kFx_buoyancy, &out->buoyancy, BF6_FXLOOK_BUOYANCY);
    f4(kFx_buoyancy_over_life, out->buoyancy_over_life, BF6_FXLOOK_BUOYANCY_OVER_LIFE);
    f1(kFx_wind_strength, &out->wind_strength, BF6_FXLOOK_WIND);
    f1(kFx_random_force, &out->random_force, BF6_FXLOOK_RANDOM_FORCE);
    f4(kFx_random_force_over_life, out->random_force_over_life, BF6_FXLOOK_RANDOM_FORCE_OVER_LIFE);
    f3(kFx_local_force, out->local_force, BF6_FXLOOK_LOCAL_FORCE);
    f4(kFx_local_force_over_life, out->local_force_over_life, BF6_FXLOOK_LOCAL_FORCE_OVER_LIFE);
    f1(kFx_direction_from_origin, &out->direction_from_origin, BF6_FXLOOK_DIRECTION_FROM_ORIGIN);
    f1(kFx_restitution, &out->restitution, BF6_FXLOOK_RESTITUTION);
    f1(kFx_restitution_variance, &out->restitution_variance, BF6_FXLOOK_RESTITUTION_VARIANCE);
    f1(kFx_friction, &out->friction, BF6_FXLOOK_FRICTION);
    f1(kFx_friction_variance, &out->friction_variance, BF6_FXLOOK_FRICTION_VARIANCE);
    f3(kFx_spawn_position, out->spawn_position, BF6_FXLOOK_SPAWN_POSITION);
    f3(kFx_spawn_scale, out->spawn_scale, BF6_FXLOOK_SPAWN_SCALE);
    f1(kFx_inner_radius, &out->inner_radius, BF6_FXLOOK_INNER_RADIUS);
    f1(kFx_outer_radius, &out->outer_radius, BF6_FXLOOK_OUTER_RADIUS);
    f1(kFx_start_zenith_angle, &out->start_zenith_angle, BF6_FXLOOK_START_ZENITH);
    f1(kFx_end_zenith_angle, &out->end_zenith_angle, BF6_FXLOOK_END_ZENITH);
    f1(kFx_sun_light_scale, &out->sun_light_scale, BF6_FXLOOK_SUN_LIGHT);
    f1(kFx_local_light_scale, &out->local_light_scale, BF6_FXLOOK_LOCAL_LIGHT);
    f1(kFx_ambient_light_scale, &out->ambient_light_scale, BF6_FXLOOK_AMBIENT_LIGHT);
    f1(kFx_received_shadow_scale, &out->received_shadow_scale, BF6_FXLOOK_RECEIVED_SHADOW);
    f1(kFx_cloud_shadow_scale, &out->cloud_shadow_scale, BF6_FXLOOK_CLOUD_SHADOW);
    f1(kFx_gnomon_backlight, &out->gnomon_backlight, BF6_FXLOOK_GNOMON_BACKLIGHT);
    f1(kFx_vertex_backlight, &out->vertex_backlight, BF6_FXLOOK_VERTEX_BACKLIGHT);
    f1(kFx_backlight_contrast, &out->backlight_contrast, BF6_FXLOOK_BACKLIGHT_CONTRAST);
    f1(kFx_shadow_radius_mult, &out->shadow_radius_mult, BF6_FXLOOK_SHADOW_RADIUS_MULT);
    i1(kFx_light_mult_type, &out->light_mult_type, BF6_FXLOOK_LIGHT_MULT_TYPE);
    i1(kFx_gnomon_rig_index, &out->gnomon_rig_index, BF6_FXLOOK_GNOMON_RIG);
    f1(kFx_camera_bias, &out->camera_bias, BF6_FXLOOK_CAMERA_BIAS);
    f1(kFx_inv_z_fade, &out->inv_z_fade, BF6_FXLOOK_INV_Z_FADE);
    i1(kFx_disable_frame_blend, &out->disable_frame_blend, BF6_FXLOOK_FRAME_BLEND);
    i1(kFx_use_right_tile, &out->use_right_tile, BF6_FXLOOK_RIGHT_TILE);
    i1(kFx_mirror, &out->mirror, BF6_FXLOOK_MIRROR);
    f1(kFx_flip_u_probability, &out->flip_u_probability, BF6_FXLOOK_FLIP_U);
    f1(kFx_flip_v_probability, &out->flip_v_probability, BF6_FXLOOK_FLIP_V);
    i1(kFx_based_on_lifetime, &out->based_on_lifetime, BF6_FXLOOK_BASED_ON_LIFETIME);
    i1(kFx_local_space, &out->local_space, BF6_FXLOOK_LOCAL_SPACE);
/* @end:fxlook_resolve */
    return authored;
}

/* THE BUDGET. See bf6_core.h for why this is here and not in the plugins.
 *
 * 3000 was measured, not picked: past it the Godot editor's frame cost is
 * dominated by particle-system presence rather than by anything on screen, and
 * every emitter is a node with its own process material whether or not the
 * distance cull is hiding it. */
/* THE FIELD NAMES, so a consumer can map a name to its bit without keeping its
 * own copy of the order. Keeping one is not a theoretical risk: the Godot
 * environment product carried a 31-name array of its own, the enum grew to 80,
 * and every over-life curve silently stopped resolving while the layer counts
 * all still looked right. */
static const char* const kFxLookNames[] = {
/* @gen:fxlook_names */
    "base_size",
    "base_size_bias",
    "size_x",
    "size_y",
    "size_z",
    "spawn_size",
    "size_over_life",
    "size_curve",
    "size_y_mult",
    "scale",
    "scale_x_mult",
    "scale_z_mult",
    "pivot_y",
    "opacity",
    "opacity_mult",
    "opacity_over_life",
    "color",
    "color_mult",
    "color0",
    "color1",
    "random_color_min",
    "random_color_max",
    "intensity",
    "temperature",
    "ramp",
    "alpha_cull",
    "rotation_speed",
    "rotation_speed_bias",
    "rotation_over_life",
    "spawn_rotation_min",
    "spawn_rotation_max",
    "spawn_rotation_speed",
    "spawn_rotation_speed_mult",
    "spawn_rotation_speed_min_mult",
    "spawn_speed",
    "spawn_speed_mult",
    "spawn_speed_min_mult",
    "spawn_speed_bias",
    "spawn_speed_curve",
    "speed_mult",
    "drag",
    "drag_min_mult",
    "drag_over_life",
    "gravity",
    "buoyancy",
    "buoyancy_over_life",
    "wind_strength",
    "random_force",
    "random_force_over_life",
    "local_force",
    "local_force_over_life",
    "direction_from_origin",
    "restitution",
    "restitution_variance",
    "friction",
    "friction_variance",
    "spawn_position",
    "spawn_scale",
    "inner_radius",
    "outer_radius",
    "start_zenith_angle",
    "end_zenith_angle",
    "sun_light_scale",
    "local_light_scale",
    "ambient_light_scale",
    "received_shadow_scale",
    "cloud_shadow_scale",
    "gnomon_backlight",
    "vertex_backlight",
    "backlight_contrast",
    "shadow_radius_mult",
    "light_mult_type",
    "gnomon_rig_index",
    "camera_bias",
    "inv_z_fade",
    "disable_frame_blend",
    "use_right_tile",
    "mirror",
    "flip_u_probability",
    "flip_v_probability",
    "based_on_lifetime",
    "local_space",
/* @end:fxlook_names */
};

const char* bf6_fx_look_field_name(int field)
{
    if (field < 0 || field >= BF6_FXLOOK_COUNT) return "";
    return kFxLookNames[field];
}

/* ONE FIELD'S VALUE, addressed by its enum. Generated, so a consumer can walk
 * every field without naming any of them - see the note in gen_fxlook.py for
 * what happens when a serialiser names fewer fields than the resolver fills. */
int bf6_fx_look_field_value(const bf6_fx_look* look, int field, float* out4)
{
    if (!look || !out4 || field < 0 || field >= BF6_FXLOOK_COUNT) return 0;
    out4[0] = out4[1] = out4[2] = out4[3] = 0.f;
    switch (field) {
/* @gen:fxlook_values */
    case BF6_FXLOOK_BASE_SIZE: out4[0] = look->base_size; return 1;
    case BF6_FXLOOK_BASE_SIZE_BIAS: out4[0] = look->base_size_bias; return 1;
    case BF6_FXLOOK_SIZE_X: out4[0] = look->size_x; return 1;
    case BF6_FXLOOK_SIZE_Y: out4[0] = look->size_y; return 1;
    case BF6_FXLOOK_SIZE_Z: out4[0] = look->size_z; return 1;
    case BF6_FXLOOK_SPAWN_SIZE: out4[0] = look->spawn_size[0]; out4[1] = look->spawn_size[1]; return 2;
    case BF6_FXLOOK_SIZE_OVER_LIFE: out4[0] = look->size_over_life[0]; out4[1] = look->size_over_life[1]; out4[2] = look->size_over_life[2]; out4[3] = look->size_over_life[3]; return 4;
    case BF6_FXLOOK_SIZE_CURVE: out4[0] = look->size_curve[0]; out4[1] = look->size_curve[1]; out4[2] = look->size_curve[2]; out4[3] = look->size_curve[3]; return 4;
    case BF6_FXLOOK_SIZE_Y_MULT: out4[0] = look->size_y_mult; return 1;
    case BF6_FXLOOK_SCALE: out4[0] = look->scale[0]; out4[1] = look->scale[1]; out4[2] = look->scale[2]; return 3;
    case BF6_FXLOOK_SCALE_X_MULT: out4[0] = look->scale_x_mult; return 1;
    case BF6_FXLOOK_SCALE_Z_MULT: out4[0] = look->scale_z_mult; return 1;
    case BF6_FXLOOK_PIVOT_Y: out4[0] = look->pivot_y; return 1;
    case BF6_FXLOOK_OPACITY: out4[0] = look->opacity; return 1;
    case BF6_FXLOOK_OPACITY_MULT: out4[0] = look->opacity_mult; return 1;
    case BF6_FXLOOK_OPACITY_OVER_LIFE: out4[0] = look->opacity_over_life[0]; out4[1] = look->opacity_over_life[1]; out4[2] = look->opacity_over_life[2]; out4[3] = look->opacity_over_life[3]; return 4;
    case BF6_FXLOOK_COLOR: out4[0] = look->color[0]; out4[1] = look->color[1]; out4[2] = look->color[2]; return 3;
    case BF6_FXLOOK_COLOR_MULT: out4[0] = look->color_mult[0]; out4[1] = look->color_mult[1]; out4[2] = look->color_mult[2]; return 3;
    case BF6_FXLOOK_COLOR0: out4[0] = look->color0[0]; out4[1] = look->color0[1]; out4[2] = look->color0[2]; return 3;
    case BF6_FXLOOK_COLOR1: out4[0] = look->color1[0]; out4[1] = look->color1[1]; out4[2] = look->color1[2]; return 3;
    case BF6_FXLOOK_RANDOM_COLOR_MIN: out4[0] = look->random_color_min[0]; out4[1] = look->random_color_min[1]; out4[2] = look->random_color_min[2]; return 3;
    case BF6_FXLOOK_RANDOM_COLOR_MAX: out4[0] = look->random_color_max[0]; out4[1] = look->random_color_max[1]; out4[2] = look->random_color_max[2]; return 3;
    case BF6_FXLOOK_INTENSITY: out4[0] = look->intensity; return 1;
    case BF6_FXLOOK_TEMPERATURE: out4[0] = look->temperature; return 1;
    case BF6_FXLOOK_RAMP: out4[0] = (float)look->ramp; return 1;
    case BF6_FXLOOK_ALPHA_CULL: out4[0] = look->alpha_cull; return 1;
    case BF6_FXLOOK_ROTATION_SPEED: out4[0] = look->rotation_speed; return 1;
    case BF6_FXLOOK_ROTATION_SPEED_BIAS: out4[0] = look->rotation_speed_bias; return 1;
    case BF6_FXLOOK_ROTATION_OVER_LIFE: out4[0] = look->rotation_over_life[0]; out4[1] = look->rotation_over_life[1]; out4[2] = look->rotation_over_life[2]; out4[3] = look->rotation_over_life[3]; return 4;
    case BF6_FXLOOK_SPAWN_ROTATION_MIN: out4[0] = look->spawn_rotation_min[0]; out4[1] = look->spawn_rotation_min[1]; out4[2] = look->spawn_rotation_min[2]; return 3;
    case BF6_FXLOOK_SPAWN_ROTATION_MAX: out4[0] = look->spawn_rotation_max[0]; out4[1] = look->spawn_rotation_max[1]; out4[2] = look->spawn_rotation_max[2]; return 3;
    case BF6_FXLOOK_SPAWN_ROTATION_SPEED: out4[0] = look->spawn_rotation_speed; return 1;
    case BF6_FXLOOK_SPAWN_ROTATION_SPEED_MULT: out4[0] = look->spawn_rotation_speed_mult[0]; out4[1] = look->spawn_rotation_speed_mult[1]; return 2;
    case BF6_FXLOOK_SPAWN_ROTATION_SPEED_MIN_MULT: out4[0] = look->spawn_rotation_speed_min_mult; return 1;
    case BF6_FXLOOK_SPAWN_SPEED: out4[0] = look->spawn_speed; return 1;
    case BF6_FXLOOK_SPAWN_SPEED_MULT: out4[0] = look->spawn_speed_mult[0]; out4[1] = look->spawn_speed_mult[1]; return 2;
    case BF6_FXLOOK_SPAWN_SPEED_MIN_MULT: out4[0] = look->spawn_speed_min_mult; return 1;
    case BF6_FXLOOK_SPAWN_SPEED_BIAS: out4[0] = look->spawn_speed_bias; return 1;
    case BF6_FXLOOK_SPAWN_SPEED_CURVE: out4[0] = look->spawn_speed_curve[0]; out4[1] = look->spawn_speed_curve[1]; out4[2] = look->spawn_speed_curve[2]; out4[3] = look->spawn_speed_curve[3]; return 4;
    case BF6_FXLOOK_SPEED_MULT: out4[0] = look->speed_mult; return 1;
    case BF6_FXLOOK_DRAG: out4[0] = look->drag; return 1;
    case BF6_FXLOOK_DRAG_MIN_MULT: out4[0] = look->drag_min_mult; return 1;
    case BF6_FXLOOK_DRAG_OVER_LIFE: out4[0] = look->drag_over_life[0]; out4[1] = look->drag_over_life[1]; out4[2] = look->drag_over_life[2]; out4[3] = look->drag_over_life[3]; return 4;
    case BF6_FXLOOK_GRAVITY: out4[0] = look->gravity; return 1;
    case BF6_FXLOOK_BUOYANCY: out4[0] = look->buoyancy; return 1;
    case BF6_FXLOOK_BUOYANCY_OVER_LIFE: out4[0] = look->buoyancy_over_life[0]; out4[1] = look->buoyancy_over_life[1]; out4[2] = look->buoyancy_over_life[2]; out4[3] = look->buoyancy_over_life[3]; return 4;
    case BF6_FXLOOK_WIND: out4[0] = look->wind_strength; return 1;
    case BF6_FXLOOK_RANDOM_FORCE: out4[0] = look->random_force; return 1;
    case BF6_FXLOOK_RANDOM_FORCE_OVER_LIFE: out4[0] = look->random_force_over_life[0]; out4[1] = look->random_force_over_life[1]; out4[2] = look->random_force_over_life[2]; out4[3] = look->random_force_over_life[3]; return 4;
    case BF6_FXLOOK_LOCAL_FORCE: out4[0] = look->local_force[0]; out4[1] = look->local_force[1]; out4[2] = look->local_force[2]; return 3;
    case BF6_FXLOOK_LOCAL_FORCE_OVER_LIFE: out4[0] = look->local_force_over_life[0]; out4[1] = look->local_force_over_life[1]; out4[2] = look->local_force_over_life[2]; out4[3] = look->local_force_over_life[3]; return 4;
    case BF6_FXLOOK_DIRECTION_FROM_ORIGIN: out4[0] = look->direction_from_origin; return 1;
    case BF6_FXLOOK_RESTITUTION: out4[0] = look->restitution; return 1;
    case BF6_FXLOOK_RESTITUTION_VARIANCE: out4[0] = look->restitution_variance; return 1;
    case BF6_FXLOOK_FRICTION: out4[0] = look->friction; return 1;
    case BF6_FXLOOK_FRICTION_VARIANCE: out4[0] = look->friction_variance; return 1;
    case BF6_FXLOOK_SPAWN_POSITION: out4[0] = look->spawn_position[0]; out4[1] = look->spawn_position[1]; out4[2] = look->spawn_position[2]; return 3;
    case BF6_FXLOOK_SPAWN_SCALE: out4[0] = look->spawn_scale[0]; out4[1] = look->spawn_scale[1]; out4[2] = look->spawn_scale[2]; return 3;
    case BF6_FXLOOK_INNER_RADIUS: out4[0] = look->inner_radius; return 1;
    case BF6_FXLOOK_OUTER_RADIUS: out4[0] = look->outer_radius; return 1;
    case BF6_FXLOOK_START_ZENITH: out4[0] = look->start_zenith_angle; return 1;
    case BF6_FXLOOK_END_ZENITH: out4[0] = look->end_zenith_angle; return 1;
    case BF6_FXLOOK_SUN_LIGHT: out4[0] = look->sun_light_scale; return 1;
    case BF6_FXLOOK_LOCAL_LIGHT: out4[0] = look->local_light_scale; return 1;
    case BF6_FXLOOK_AMBIENT_LIGHT: out4[0] = look->ambient_light_scale; return 1;
    case BF6_FXLOOK_RECEIVED_SHADOW: out4[0] = look->received_shadow_scale; return 1;
    case BF6_FXLOOK_CLOUD_SHADOW: out4[0] = look->cloud_shadow_scale; return 1;
    case BF6_FXLOOK_GNOMON_BACKLIGHT: out4[0] = look->gnomon_backlight; return 1;
    case BF6_FXLOOK_VERTEX_BACKLIGHT: out4[0] = look->vertex_backlight; return 1;
    case BF6_FXLOOK_BACKLIGHT_CONTRAST: out4[0] = look->backlight_contrast; return 1;
    case BF6_FXLOOK_SHADOW_RADIUS_MULT: out4[0] = look->shadow_radius_mult; return 1;
    case BF6_FXLOOK_LIGHT_MULT_TYPE: out4[0] = (float)look->light_mult_type; return 1;
    case BF6_FXLOOK_GNOMON_RIG: out4[0] = (float)look->gnomon_rig_index; return 1;
    case BF6_FXLOOK_CAMERA_BIAS: out4[0] = look->camera_bias; return 1;
    case BF6_FXLOOK_INV_Z_FADE: out4[0] = look->inv_z_fade; return 1;
    case BF6_FXLOOK_FRAME_BLEND: out4[0] = (float)look->disable_frame_blend; return 1;
    case BF6_FXLOOK_RIGHT_TILE: out4[0] = (float)look->use_right_tile; return 1;
    case BF6_FXLOOK_MIRROR: out4[0] = (float)look->mirror; return 1;
    case BF6_FXLOOK_FLIP_U: out4[0] = look->flip_u_probability; return 1;
    case BF6_FXLOOK_FLIP_V: out4[0] = look->flip_v_probability; return 1;
    case BF6_FXLOOK_BASED_ON_LIFETIME: out4[0] = (float)look->based_on_lifetime; return 1;
    case BF6_FXLOOK_LOCAL_SPACE: out4[0] = (float)look->local_space; return 1;
/* @end:fxlook_values */
    default: return 0;
    }
}

int bf6_fx_look_field_is_int(int field)
{
    switch (field) {
/* @gen:fxlook_ints */
    case BF6_FXLOOK_RAMP:
    case BF6_FXLOOK_LIGHT_MULT_TYPE:
    case BF6_FXLOOK_GNOMON_RIG:
    case BF6_FXLOOK_FRAME_BLEND:
    case BF6_FXLOOK_RIGHT_TILE:
    case BF6_FXLOOK_MIRROR:
    case BF6_FXLOOK_BASED_ON_LIFETIME:
    case BF6_FXLOOK_LOCAL_SPACE:
/* @end:fxlook_ints */
        return 1;
    default: return 0;
    }
}

int bf6_fx_look_authored(const bf6_fx_look* look, int field)
{
    if (!look || field < 0 || field >= BF6_FXLOOK_COUNT) return 0;
    return (look->has[(unsigned)field >> 6] >> ((unsigned)field & 63)) & 1u;
}

int bf6_fx_budget_default(void) { return 3000; }

int bf6_fx_budget_keep(int index, int total, int budget)
{
    if (budget <= 0) budget = bf6_fx_budget_default();
    if (total <= 0 || index < 0 || index >= total) return 0;
    if (total <= budget) return 1;
    /* Bresenham selection: exactly `budget` indices are kept, spread evenly
     * over the range. The alternative - index < budget - keeps a contiguous
     * prefix, and since the layer list is ordered by effect that empties most
     * of the map while leaving one effect fully populated. */
    const long long a = (long long)index * (long long)budget / (long long)total;
    const long long b = ((long long)index + 1) * (long long)budget / (long long)total;
    return a != b ? 1 : 0;
}
