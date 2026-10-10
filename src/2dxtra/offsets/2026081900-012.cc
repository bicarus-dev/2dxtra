versions.push_back({
    .GAME_VERSION             = "2026081900-012",

    .DLL_CODE_SIZE            = 0x0bc0a00,
    .DLL_ENTRYPOINT           = 0x0a4fd5c,
    .DLL_IMAGE_SIZE           = 0xba81000,

    .SOFT_REV_PATCH           = base + 0x08815d9, // call replaced by `mov byte ptr [rdi+5], rev`
    .INPUT_POLL_FN            = base + 0x09ad3b0, // IO::InputManagerIIDX vftable slot 3
    .ARENA_PHASE_PATCH        = base + 0x04d0fd0, // callee of the arena phase clamp
    .MUSIC_SELECT_CTOR        = base + 0x05364d0, // builds the mlist_sel/thumbnail textures
    .DAN_SELECT_CTOR          = base + 0x08001f0, // CDanSelectScene vftable slot 13 (scene enter)
    .SCENE_DTOR               = base + 0x089d100, // prologue pattern [pattern]
    .STAGE_RESULT_FN          = base + 0x085f0a0, // CStageResultScene vftable slot 13 (scene enter)
    .RESET_STATE_FN           = base + 0x04de4c0, // clears one player's game data block
    .MSELECT_GENRE_C          = base + 0x0535310, // renders the hovered entry's genre and title
    .MSELECT_GENRE_B          = base + 0x05353a1, // cmp music_entry::texture_genre, 0 (7 bytes, patched to jmp)
    .MSELECT_GENRE_A          = base + 0x053547d, // call to the wide genre text renderer
    .MSELECT_GENRE_TEXT_FN    = base + 0x051f570, // callee of MSELECT_GENRE_A
    .INIT_TEXT_RENDER_FN      = base + 0x051e5e0, // debug text renderer used for the boot progress line
    .BOOT_MODEL_TEXT_CALL     = base + 0x087234b, // call that draws the boot model string
    .TITLE_MODEL_TEXT_CALL    = base + 0x086989c, // call that draws the title-screen model string
    .TEXT_INIT_FN             = base + 0x0344880, // initialises the text property block (first callee)
    .TEXT_RENDER_FN           = base + 0x051e960, // public wrapper around the ASCII text draw routine
    .RESULT_ARTIST_FN         = base + 0x084f1b9, // call that draws the result artist text
    .LOAD_CHART_FN_B          = base + 0x075aa30, // reads one .1 chart into the scratch buffer
    .LOAD_CHART_FN_A          = base + 0x075ab50, // sole caller of LOAD_CHART_FN_B
    .CHART_ANALYZE_FN         = base + 0x07598c0, // boot-time chart load + analyze pass
    .CHART_ANALYZE_RESULT     = base + 0x0759ae3, // instruction after the chart fread (rax = bytes read, rdi = buffer)
    .CHART_CALC_RADAR_FN      = base + 0x075a3e0, // prologue pattern [pattern]
    .GET_APP_CONFIG           = base + 0x086e890, // returns CApplicationConfig
    .GET_MUSIC_DATA           = base + 0x0884eb0, // returns the music_data.bin blob
    .LOAD_AUDIO_FN            = base + 0x09c2e30, // prologue pattern [pattern]
    .GET_SOUND_ENTRY_FN       = base + 0x09c29d0, // prologue pattern [pattern]
    .XRPC_APPLY_FN            = base + 0x09ec5b0, // the game's wrapper around avs2-ea3 xrpc_apply
    .REG_DISPATCH_FN          = base + 0x08492b0, // builds and fires the music.reg request
    .REG_PATCH_ADDR           = base + 0x084a1a5, // call that actually queues the score save request
    .SCORE_INVALID_FN         = base + 0x07612e0, // shared by the result scene and the score save request
    .DAN_SAVE_FN              = base + 0x04e6860, // prologue pattern [pattern]
    .EAAPPLI_SAVE_FN          = base + 0x04ead90, // prologue pattern [pattern]
    .MDATA_LOAD_FN            = base + 0x0867120, // CMonitorCheckScene vftable slot 15 (runs after music_data is loaded)
    .IS_BTN_DOWN_FN_A         = base + 0x0751b8d, // cmp autoplay flag inside the `button pressed` helper
    .IS_BTN_DOWN_FN_B         = base + 0x0751c2d, // cmp autoplay flag inside the `button held` helper
    .AUTO_BEAM_PATCH          = base + 0x0838c8b, // cmp autoplay flag in the lane beam renderer
    .AUTO_BEAM_FN             = base + 0x0838a30, // lane beam renderer; rcx -> per-player block whose first dword is the player index
    .CARD_OUT_VFUNC           = base + 0x0ca8890, // CCardOutScene vftable slot 0
    .RENDERER_PRESENT_FN      = base + 0x08ab230, // shared presentation routine after frame rendering
    .APPLY_RANDOM_FN          = base + 0x07564b0, // prologue pattern [pattern]
    .DARK_MODE_PATCH          = base + 0x0835428, // short `je` over the black frame tint assignment, patched to two NOPs
    .MEASURE_PATCH            = base + 0x0756cf9, // branch over the measure bar draw, forced unconditional
    .BPM_BAR_PATCH            = base + 0x083479a, // start of the bpm gradient draw, replaced by a jmp past it
    .BPM_BAR_PATCH_JMP        =        0x00000ba, // relative jump to 0x834859
    .PLAY_FIELD_LOAD          = base + 0x074f3b0, // fills the play field note table
    .RETRY_CHECK_A            = base + 0x0842b48, // call testing the EFFECT button
    .RETRY_CHECK_B            = base + 0x0842b5a, // call testing the VEFX button
    .GHOST_TARGET_FN          = base + 0x08395e9, // call that renders the '+????' ghost text
    .GRAPH_TARGET_FN          = base + 0x096fff5, // call that renders the 'TARGET:+????' text
    .GRAPH_CONDITION          = base + 0x096ffad, // gate on whether the TARGET text is drawn
    .FAIL_ANIMATION_FN        = base + 0x084335b, // call that starts the stage failed animation (dl = play it)
    .FAIL_PLAY_SFX_FN         = base + 0x084338a, // call that plays the stage failed sound effect (rcx = sound id)
    .FAIL_DURATION_JMP        = base + 0x0842d00, // `jl` deciding whether the failed animation is still running
    .WNDPROC_FN               = base + 0x086fd80, // PropClassWindowProc
    .TIMING_HOOK_FN           = base + 0x0754500, // jump-table dispatcher that installs the judge windows
    .ATTRACT_SELECT_FN        = base + 0x07d3ca0, // picks the next attract demo chart

    .IS_SEPARATE_SCRATCH_FN   = base + 0x073d670, // whether separate FAST/SLOW for scratch option is enabled
    .SPRITE_DRAW_FN           = base + 0x034ac50, // named sprite draw helper used for s_fast and s_slow
    .JUDGE_APPLY_FN           = base + 0x0753e80, // applies a note judgment; prologue pattern [pattern]
    .JUDGE_DISPLAY_FN         = base + 0x083cd50, // updates judge, combo and FAST/SLOW state; prologue pattern [pattern]
    .JUDGE_DRAW_FS_KEYS_FN    = base + 0x083c520, // fast/slow renderer sharing the scratch renderer's caller
    .JUDGE_DRAW_FS_SC_FN      = base + 0x083c810, // renderer referencing s_fast and s_slow
    .JUDGE_DISPLAY_INIT_FN    = base + 0x083cae0, // initializes judgment display state and judge_great_yellow sprites
    .JUDGE_PRESS_RETURN       = base + 0x0754ab1, // return after general timing judgment call; continuation pattern [pattern]
    .JUDGE_RELEASE_RETURN     = base + 0x0753a5a, // return after CN release timing judgment call; continuation pattern [pattern]
    .CN_RELEASE_INPUT_RETURN  = base + 0x07538e0, // test al, al; r13=context, r15=player, rsi=lane
    .CN_RELEASE_TIMING_RETURN = base + 0x075395d, // mov edi, eax; add eax, -2
    .CN_HEAD_TIMING_RETURN    = base + 0x075498f, // rdi=context, r14=player, rbp=lane, eax=grade
    .CHART_NOTE_COUNT_SETUP   = base + 0x0759773, // test si, si; r15=event type, r13=chart player
    .CN_END_SPRITE_RETURN     = base + 0x0844869, // normal CN/backspin tail sprite
    .BSS_END_SPRITE_RETURN    = base + 0x084468b, // linked backspin tail sprite
    .HI_SPEED_RESET_PATCH     = base + 0x083d023, // xor byte ptr [rsi+28h], 1; native double-START branch
    .HI_SPEED_RESET_CONTINUE  = base + 0x083d13a, // mov rcx, rsi; call lane/green-number predicate
    .HI_SPEED_ADJUST_CALL     = base + 0x083e6f6, // call 0x83ea40; RCX = controller, 5 bytes
    .GET_PLAYER_OPTIONS       = base + 0x07b8270, // runtime options singleton accessor
    .IS_DOUBLE_PLAY           = base + 0x0760ec0, // SP/DP selector used by option accessors
    .GET_HI_SPEED_MODE        = base + 0x07ca870, // selected option block +0x1c
    .GET_SAVED_HI_SPEED       = base + 0x07c8950, // option block +4 divided by native speed scale
    .SET_HI_SPEED             = base + 0x075c400, // preserves interpolation, clamps target speed

    .SRAN_CANDIDATE_CHECK   = base + 0x0755856, // S-RAN note placement algorithm; used to override with H-RAN

    .RESULT_OPTIONS_FN      = base + 0x07c9b90, // option text: result screen, Analyze Play on subscreen
    .RETRY_OPTIONS_FN       = base + 0x07c92e0, // option text: subscreen chart-retry dialog
    .PACEMAKER_OPTIONS_FN   = base + 0x09795e0, // option text: pacemaker panel

    .RANDOM_SEQUENCE_DRAW_FN  = base + 0x0837b40, // native Random Sequence renderer
    .RANDOM_TICKET_DATA_FN    = base + 0x07b8380, // RandomLane::CRandomLaneTicketGameData getter

    .PLAY_SCENE_DRAW_FN       = base + 0x0837f20, // updates both sides' key lights and turntable sprites
    .KEY_LIGHT_POSITION       = base + 0x0838bb9, // call rsi: button-light position, R14 = display, RCX = sprite, EDX/R8D = x/y, EBX = half-width
    .BGA_UPDATE_FN            = base + 0x082a890,

    .GAUGE_RESET              = base + 0x08a8270, // initializes both players' gauges for a new attempt
    .GAUGE_DELTA_SETUP        = base + 0x08a8da0, // calculates native delta magnitudes from P1/P2 note counts
    .GAUGE_JUDGMENT           = base + 0x08a7960, // applies a judgment to a player's gauge (player, judgment 0-5)
    .GAUGE_TICK               = base + 0x08a8f00, // per-tick gauge update (playback tick index)
    .GAUGE_APPLY_DELTA        = base + 0x08a8c20, // applies a signed raw gauge delta and handles failure (player, delta)
    .GAUGE_TICK_MS            = base + 0x075b470, // returns the duration of one native playback tick in milliseconds
    .GAUGE_VALUES             = base + 0xb187310, // two 20-byte blocks: raw gauge value and four native delta magnitudes
    .GAUGE_SPECIAL_STATE      = base + 0xb182718, // two native shared gauge-info entries (16 bytes each)
    .GAUGE_ARTWORK            = base + 0x0836ae6, // artwork-selection mid-hook: ESI = player, EBP/R15 = renderer-local style
    .BM2D_RENDERER            = base + 0x034d000, // BM2D renderer getter; native HLS shader pointer at +98464
    .GAUGE_RESULT_CLEAR       = base + 0x0865920, // result/submission clear type (player); 0 = NO PLAY, 7 = FULL COMBO
    .GAUGE_DELTA_MAGNITUDE    = base + 0x08a71e0, // calculates one native delta magnitude (player, kind, note count)

    // Per-column note coloring: ordinary-note batches and CN/HCN sprites.
    .NOTE_DRAW_FN             = base + 0x0756c30, // draws one note/CN using player and note-column metadata
    .NOTE_BATCHES             = base + 0x0353050, // getter for two 86096-byte ordinary-note batches
    .NOTE_SPRITE_CREATED      = base + 0x034b720, // indexed sprite creation return: RAX = sprite
    .NOTE_BATCH_DRAW          = base + 0x03531cf, // renderer's inlined DrawPrimitiveUP call: RBX = batch +0x1C, RCX = device

    // offsets: data
    .GAME_MODEL               = base + 0x0f81d40, // the mutable copy of the ea3 model string
    .GAME_STATE               = base + 0xabd7960, // state block; p1_active/p2_active at +0x10/+0x14 pin it down
    .INPUT_MANAGER            = base + 0xb19a270, // the InputManagerIIDX singleton (its vftable pointer is written here)
    .PLAYER_BLOCK             = base + 0x3001488, // base of player 0's game data block
    .SCORES_P1                = base + 0x302f01c, // player 0 score table (player block + the score updater's displacement)
    .SCORES_P2                = base + 0x6b3f3bc, // player 1 score table
    .RIVAL_SCORES_P1          = base + 0x37f73c0, // player 0 rival score table
    .RIVAL_SCORES_P2          = base + 0x7307760, // player 1 rival score table
    .MAX_ENTRIES              =        0x00084d0, // music entries per play style in the score tables
    .D3D9_DEVICE              = base + 0xabd85a0, // IDirect3DDevice9* field assigned after CreateDeviceEx at 0x86f630 (manager +0xe0)
    .AUTO_PLAY                = base + 0xa99475c, // autoplay flag inside the play data block
    .RANDOM_DATA              = base + 0xa6ef530, // object holding the post-random lane order
    .PLAY_STATE               = base + 0xa6ed450, // per-play score/note counters and the pacemaker target
    .PLAY_SESSION             = base + 0xaaa99f0, // gameplay session block (pacemaker type, personal best, in-game flag)
    .DEAD_STATE               = base + 0xaaa95a0, // per-player alive flags from the failure handler's shared getter
    .PLAY_NOTE_POS            = base + 0x0f66550, // SP then DP; each side has seven key left edges and scratch
    .HI_SPEED_STATE           = base + 0xa9ad680, // singleton returned by 0x75c5b0

    .AUDIO_QUEUE_KEY_APPEND   = base + 0x0757f23, // RBX = queue, sample written, count not yet incremented
    .AUDIO_QUEUE_BGM_APPEND   = base + 0x0757ffc, // RCX = queue, sample written, count not yet incremented
    .AUDIO_QUEUE_PLAY_RETURN  = base + 0x075803f,
    .AUDIO_PLAY_FN            = base + 0x09c3a60, // bank/sample playback, before native voice lookup/start
    .AUDIO_PLAY_RETURN        = base + 0x04f82a5, // ordinary play wrapper's caller is at entry RSP+0x30
    .AUDIO_GET_VOICE_FN       = base + 0x09c2d50, // returns an owning MSVC shared_ptr through RDX
    .AUDIO_MIX_GAIN           = base + 0x04b9adb, // XMM8 = native gain * fade; R14 = voice + 8
    .AUDIO_GLOBAL_MIX         = base + 0x04acfe2, // output limiter: XMM1 gain, XMM2/3 positive/negative bounds
    .CUSTOM_AUDIO_START       = base + 0x09c3b33, // RAX = cache slot; RSI = manager; EDI = bank; EBX = sample; BPL = loop
    .CUSTOM_AUDIO_LAYER_GAIN  = base + 0x08a65f0, // default select layer target, before native fades
    .CUSTOM_AUDIO_LOAD        = base + 0x09c38a0, // native system-sound file loader, including cache replacement
    .CUSTOM_AUDIO_NAMES       = base + 0x0d05890, // native SD9 basename table, 648 bytes per record
});
