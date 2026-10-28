"""Explicit raw-VP, clip-depth and framebuffer sample-coordinate contract."""
from dataclasses import dataclass
import numpy as np


@dataclass(frozen=True)
class Viewport:
    x: float
    y: float
    width: float
    height: float
    min_depth: float
    max_depth: float
    minus_w: bool

    def __post_init__(self):
        if not np.isfinite(self.values()).all() or self.width <= 0 or not self.height or self.min_depth == self.max_depth:
            raise ValueError('Invalid reprojection viewport')

    def values(self):
        return [self.x, self.y, self.width, self.height, self.min_depth, self.max_depth]


def reproject(current, previous, cv, pv, x, y, depth, object_history=None):
    """Return previous framebuffer sample x/y/depth and validity mask.

    x/y are pixel index + measured MSAA offset, or a declared approximation.
    Identity object history assumes a fixed world point. Moving geometry needs
    a current-world -> previous-world transform, unavailable from camera alone.
    """
    current, previous = np.asarray(current, dtype='float64'), np.asarray(previous, dtype='float64')
    if current.shape != (4, 4) or previous.shape != (4, 4) or not np.isfinite([current, previous]).all():
        raise ValueError('Invalid camera matrix')
    x, y, depth = np.broadcast_arrays(np.asarray(x, dtype='float64'), np.asarray(y, dtype='float64'), np.asarray(depth, dtype='float64'))
    shape = x.shape
    x, y, depth = x.ravel(), y.ravel(), depth.ravel()
    normalized = (depth-cv.min_depth)/(cv.max_depth-cv.min_depth)
    clip_z = normalized*2-1 if cv.minus_w else normalized
    ndc = np.stack((2*(x-cv.x)/cv.width-1, 2*(y-cv.y)/cv.height-1, clip_z, np.ones_like(depth)))
    history = np.eye(4) if object_history is None else np.asarray(object_history, dtype='float64')
    if history.shape != (4, 4) or not np.isfinite(history).all():
        raise ValueError('Invalid object history')
    q = previous @ history @ np.linalg.solve(current, ndc)
    valid = np.isfinite(ndc).all(axis=0) & (depth != 1) & (normalized >= 0) & (normalized <= 1) & (q[3] > 1e-8)
    valid &= (x >= cv.x) & (x < cv.x+cv.width) & (y >= min(cv.y, cv.y+cv.height)) & (y < max(cv.y, cv.y+cv.height))
    q = q[:3]/np.where(np.abs(q[3]) > 1e-12, q[3], np.nan)
    valid &= np.isfinite(q).all(axis=0) & (np.abs(q[0]) <= 1) & (np.abs(q[1]) <= 1) & (q[2] >= (-1 if pv.minus_w else 0)) & (q[2] <= 1)
    px, py = pv.x+(q[0]+1)*pv.width/2, pv.y+(q[1]+1)*pv.height/2
    pz = pv.min_depth+((q[2]+1)/2 if pv.minus_w else q[2])*(pv.max_depth-pv.min_depth)
    valid &= (px >= pv.x) & (px < pv.x+pv.width) & (py >= min(pv.y, pv.y+pv.height)) & (py < max(pv.y, pv.y+pv.height))
    return tuple(a.reshape(shape) for a in (px, py, pz, valid))
