import sys, torch, numpy as np
sys.path.insert(0, '../beat_this_src')
from beat_this.inference import load_model

model = load_model('small0.ckpt', 'cpu').eval()
print('params', sum(p.numel() for p in model.parameters()))

class Wrap(torch.nn.Module):
    def __init__(self, m):
        super().__init__(); self.m = m
    def forward(self, x):
        o = self.m(x)
        return o['beat'], o['downbeat']

w = Wrap(model).eval()
dummy = torch.randn(1, 1500, 128)
torch.onnx.export(w, dummy, 'beat_this_small0.onnx', input_names=['spect'], output_names=['beat', 'downbeat'],
                  dynamic_axes={'spect': {1: 'T'}, 'beat': {1: 'T'}, 'downbeat': {1: 'T'}}, opset_version=17, dynamo=False)
print('exported')
import onnxruntime as ort
s = ort.InferenceSession('beat_this_small0.onnx', providers=['CPUExecutionProvider'])
for T in (1500, 700, 321):
    x = torch.randn(1, T, 128)
    with torch.no_grad():
        ref = w(x)
    out = s.run(None, {'spect': x.numpy()})
    print(T, 'max abs diff beat', float(np.abs(out[0] - ref[0].numpy()).max()), 'downbeat', float(np.abs(out[1] - ref[1].numpy()).max()))
