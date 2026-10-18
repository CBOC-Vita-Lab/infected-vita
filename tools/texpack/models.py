"""The upscaler networks, written out so no basicsr/realesrgan package is needed (they pin old torchvision versions).

    load_model(name, models_dir) -> (torch.nn.Module, scale)

Networks: SRVGGNetCompact (realesr-general-x4v3, realesr-animevideov3, 2xHDCubeCompact, 4xHDCube4Compact) and
RRDBNet (RealESRGAN_x4plus). The weights are downloaded by fetch_models.sh; they are not part of the repository.
Layer names follow the published checkpoints so the state dicts load unchanged.
"""
import os

import torch
import torch.nn as nn
import torch.nn.functional as F


class SRVGGNetCompact(nn.Module):
    """A plain VGG-style conv stack with PReLU, a pixel shuffle tail and a nearest-neighbour residual."""

    def __init__(self, num_in_ch=3, num_out_ch=3, num_feat=64, num_conv=16, upscale=4):
        super().__init__()
        self.upscale = upscale
        body = [nn.Conv2d(num_in_ch, num_feat, 3, 1, 1), nn.PReLU(num_parameters=num_feat)]
        for _ in range(num_conv):
            body += [nn.Conv2d(num_feat, num_feat, 3, 1, 1), nn.PReLU(num_parameters=num_feat)]
        body.append(nn.Conv2d(num_feat, num_out_ch * upscale * upscale, 3, 1, 1))
        self.body = nn.ModuleList(body)
        self.up = nn.PixelShuffle(upscale)

    def forward(self, x):
        out = x
        for layer in self.body:
            out = layer(out)
        out = self.up(out)
        return out + F.interpolate(x, scale_factor=self.upscale, mode='nearest')


class ResidualDenseBlock(nn.Module):
    def __init__(self, nf=64, gc=32):
        super().__init__()
        self.conv1 = nn.Conv2d(nf, gc, 3, 1, 1)
        self.conv2 = nn.Conv2d(nf + gc, gc, 3, 1, 1)
        self.conv3 = nn.Conv2d(nf + 2 * gc, gc, 3, 1, 1)
        self.conv4 = nn.Conv2d(nf + 3 * gc, gc, 3, 1, 1)
        self.conv5 = nn.Conv2d(nf + 4 * gc, nf, 3, 1, 1)
        self.lrelu = nn.LeakyReLU(negative_slope=0.2, inplace=True)

    def forward(self, x):
        x1 = self.lrelu(self.conv1(x))
        x2 = self.lrelu(self.conv2(torch.cat((x, x1), 1)))
        x3 = self.lrelu(self.conv3(torch.cat((x, x1, x2), 1)))
        x4 = self.lrelu(self.conv4(torch.cat((x, x1, x2, x3), 1)))
        x5 = self.conv5(torch.cat((x, x1, x2, x3, x4), 1))
        return x5 * 0.2 + x


class RRDB(nn.Module):
    def __init__(self, nf, gc=32):
        super().__init__()
        self.rdb1 = ResidualDenseBlock(nf, gc)
        self.rdb2 = ResidualDenseBlock(nf, gc)
        self.rdb3 = ResidualDenseBlock(nf, gc)

    def forward(self, x):
        return self.rdb3(self.rdb2(self.rdb1(x))) * 0.2 + x


class RRDBNet(nn.Module):
    def __init__(self, num_in_ch=3, num_out_ch=3, scale=4, num_feat=64, num_block=23, num_grow_ch=32):
        super().__init__()
        self.scale = scale
        self.conv_first = nn.Conv2d(num_in_ch, num_feat, 3, 1, 1)
        self.body = nn.Sequential(*[RRDB(num_feat, num_grow_ch) for _ in range(num_block)])
        self.conv_body = nn.Conv2d(num_feat, num_feat, 3, 1, 1)
        self.conv_up1 = nn.Conv2d(num_feat, num_feat, 3, 1, 1)
        self.conv_up2 = nn.Conv2d(num_feat, num_feat, 3, 1, 1)
        self.conv_hr = nn.Conv2d(num_feat, num_feat, 3, 1, 1)
        self.conv_last = nn.Conv2d(num_feat, num_out_ch, 3, 1, 1)
        self.lrelu = nn.LeakyReLU(negative_slope=0.2, inplace=True)

    def forward(self, x):
        feat = self.conv_first(x)
        feat = feat + self.conv_body(self.body(feat))
        feat = self.lrelu(self.conv_up1(F.interpolate(feat, scale_factor=2, mode='nearest')))
        feat = self.lrelu(self.conv_up2(F.interpolate(feat, scale_factor=2, mode='nearest')))
        return self.conv_last(self.lrelu(self.conv_hr(feat)))


# name -> (file, kind, kwargs, scale)
CATALOG = {
    'general-x4v3': ('realesr-general-x4v3.pth', 'vgg', dict(num_feat=64, num_conv=32, upscale=4), 4),
    'general-wdn-x4v3': ('realesr-general-wdn-x4v3.pth', 'vgg', dict(num_feat=64, num_conv=32, upscale=4), 4),
    'animevideov3': ('realesr-animevideov3.pth', 'vgg', dict(num_feat=64, num_conv=16, upscale=4), 4),
    'x4plus': ('RealESRGAN_x4plus.pth', 'rrdb', dict(num_feat=64, num_block=23, num_grow_ch=32, scale=4), 4),
    'hdcube4-compact': ('4xHDCube4Compact.pth', 'vgg', dict(num_feat=64, num_conv=16, upscale=4), 4),
    'hdcube2-compact': ('2xHDCubeCompact_500k.pth', 'vgg', dict(num_feat=64, num_conv=16, upscale=2), 2),
}


def _state_dict(path):
    data = torch.load(path, map_location='cpu', weights_only=True)
    for key in ('params_ema', 'params', 'state_dict', 'model'):
        if isinstance(data, dict) and key in data and isinstance(data[key], dict):
            return data[key]
    return data


def load_model(name, models_dir):
    file, kind, kwargs, scale = CATALOG[name]
    path = os.path.join(models_dir, file)
    state = _state_dict(path)
    if kind == 'vgg':
        # the layer count is in the checkpoint: body.<2*(num_conv+1)>.weight is the last conv
        convs = sorted(int(k.split('.')[1]) for k in state if k.startswith('body.') and k.endswith('.weight') and state[k].ndim == 4)
        kwargs = dict(kwargs)
        kwargs['num_conv'] = len(convs) - 2
        kwargs['num_feat'] = state['body.0.weight'].shape[0]
        net = SRVGGNetCompact(**kwargs)
    else:
        net = RRDBNet(**kwargs)
    net.load_state_dict(state, strict=True)
    net.eval()
    return net, scale


@torch.no_grad()
def run_model(net, rgb01, tile=0, pad=8):
    """rgb01: float32 tensor (3,H,W) in [0,1]. Returns (3,H*s,W*s). Tiles with overlap when `tile` > 0."""
    x = rgb01.unsqueeze(0)
    if tile <= 0 or max(x.shape[2], x.shape[3]) <= tile:
        return net(x)[0]
    scale = None
    h, w = x.shape[2:]
    out = None
    for y0 in range(0, h, tile):
        for x0 in range(0, w, tile):
            ya, yb = max(0, y0 - pad), min(h, y0 + tile + pad)
            xa, xb = max(0, x0 - pad), min(w, x0 + tile + pad)
            o = net(x[:, :, ya:yb, xa:xb])
            if out is None:
                scale = o.shape[2] // (yb - ya)
                out = torch.zeros(1, 3, h * scale, w * scale, device=x.device)
            ys, xs = (y0 - ya) * scale, (x0 - xa) * scale
            hh, ww = min(tile, h - y0) * scale, min(tile, w - x0) * scale
            out[:, :, y0 * scale:y0 * scale + hh, x0 * scale:x0 * scale + ww] = o[:, :, ys:ys + hh, xs:xs + ww]
    return out[0]
