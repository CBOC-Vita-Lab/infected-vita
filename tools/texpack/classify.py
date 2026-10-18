"""Texture classes for the upscaling and packing policy.

The census gives every distinct image (texel data) a class from its names and, when a run-time harvest of
INFECTED_TEX_DUMP JSON files is available, from how the renderer actually drew it (through-mode = 2D, otherwise 3D).

    ui         2D screen art: HUD, menus, fonts, logos, backgrounds of the front end
    character  textures whose palette the game recolours at run time (avatars, infected, civilians)
    fx         particles and glows (soft alpha, mostly additive)
    world      everything else drawn in 3D: buildings, streets, props, vehicles
"""
import glob
import json
import os
import re

UI_PREFIXES = (
    'ui_', 'icon_', 'button', 'key_', 'selector', 'swatch', 'overload', 'vitality', 'ammo', 'missions', 'medal',
    'upgrades', 'tutorial', 'aa_', 'font', 'b_edit', 'ballistic_', 'limited_', 'timed_', 'weapon_', 'weapons_',
    'civilians_', 'lock_icon', 'message_', 'album_', 'loading_', 'infected_logo', 'biohazard_logo', 'custommask',
    'back_', 'marketing', 'logo', 'tiff', 'splash', 'cutscene_', 'hud_', 'map_')
CHARACTER_PREFIXES = ('avatar_', 'infected_', 'civilian_', 'slipknot_', 'bloodrayne', 'santa', 'special')
FX_PREFIXES = ('fx_', 'fxn_', 'fxr_', 'fx')
# 2D art whose palette the game still rewrites at run time (the HUD minimap): a recolour record in the pack, like characters
RECOLOUR_NAMES = ('map_satellite',)


def name_class(name):
    n = name.lower()
    if n.startswith('infected_logo'):
        return 'ui'
    if n.startswith(CHARACTER_PREFIXES) and not n.startswith('civilians_'):
        return 'character'
    if n.startswith(UI_PREFIXES):
        return 'ui'
    if n.startswith(FX_PREFIXES) and not n.startswith('fx_decal'):
        return 'fx'
    return 'world'


def load_observed(rt_dir):
    """key -> 'ui' | 'world' | 'both' from the JSON files of a run-time harvest (INFECTED_TEX_DUMP directories)."""
    seen = {}
    for path in glob.glob(os.path.join(rt_dir, '*', '*.json')):
        try:
            j = json.load(open(path))
        except (OSError, ValueError):
            continue
        t, d = bool(j.get('used_through')), bool(j.get('used_3d'))
        if not (t or d):
            continue
        k = 'both' if t and d else ('ui' if t else 'world')
        old = seen.get(j['key'])
        seen[j['key']] = k if old in (None, k) else 'both'
    return seen


def observed_image_keys(rt_dir):
    """Image keys (texels only) of every texture the renderer drew, whatever palette it had."""
    out = set()
    for path in glob.glob(os.path.join(rt_dir, '*', '*.json')) if rt_dir else []:
        try:
            j = json.load(open(path))
        except (OSError, ValueError):
            continue
        if j.get('used_through') or j.get('used_3d'):
            out.add(j.get('image_key'))
    return out


def image_class(names, keys, observed=None):
    """Class of an image: the observed draw kind wins over the name for ui/world, the name decides the rest."""
    guess = name_class(sorted(names)[0])
    if observed:
        kinds = {observed[k] for k in keys if k in observed}
        if kinds == {'ui'}:
            return 'ui' if guess != 'character' else 'character'
        # a ui_ name is HUD art even when the game draws it through the 3D pipeline (the font atlases, reticules, map dots are
        # drawn as transformed 3D triangles): it must keep the UI policy (pixel-exact scalers, uncompressed small sprites)
        if kinds == {'world'} and guess == 'ui' and not min(n.lower() for n in names).startswith('ui_') and \
                not any(n.lower().startswith(('ui_', 'font')) for n in names):
            return 'world'
    return guess


def runtime_recoloured(names):
    return any(n.lower().startswith(RECOLOUR_NAMES) for n in names)


def recoloured_images(cat, rt_dir):
    """Image ids whose texels the renderer saw with a palette that is not on the disc (full key unknown, image key known):
    the game recolours them at run time."""
    keys = {t['key'] for t in cat['textures']}
    by_image_key = {t['image_key']: t['image'] for t in cat['textures']}
    found = set()
    for path in glob.glob(os.path.join(rt_dir, '*', '*.json')) if rt_dir else []:
        try:
            j = json.load(open(path))
        except (OSError, ValueError):
            continue
        if j['key'] not in keys and j.get('image_key') in by_image_key:
            found.add(by_image_key[j['image_key']])
    return found


def wrap_hint(cls):
    """Whether the picture should be padded with wrapped content (repeating textures) or reflected."""
    return cls == 'world'
