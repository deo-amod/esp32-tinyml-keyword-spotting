"""DS-CNN sized for the classic ESP32 (~2.4 M MACs, ~13 k parameters)."""
import keras
from keras import layers

import kws_config as cfg


def _ds_block(x, filters, stride):
    x = layers.DepthwiseConv2D(3, strides=stride, padding="same", use_bias=False)(x)
    x = layers.BatchNormalization()(x)
    x = layers.ReLU()(x)
    x = layers.Conv2D(filters, 1, use_bias=False)(x)
    x = layers.BatchNormalization()(x)
    return layers.ReLU()(x)


def build_model(filters=48, blocks=4, dropout=0.2):
    # Input is the raw int8 feature value (-128..127) as float; the firmware
    # requantises it to the model's input scale/zero-point.
    inp = keras.Input((cfg.FEATURE_FRAMES, cfg.FEATURE_CHANNELS, 1), name="features")
    x = layers.Conv2D(filters, (10, 4), strides=(2, 2), padding="same", use_bias=False)(inp)
    x = layers.BatchNormalization()(x)
    x = layers.ReLU()(x)
    for i in range(blocks):
        x = _ds_block(x, filters, stride=2 if i == 0 else 1)
    x = layers.AveragePooling2D(pool_size=(x.shape[1], x.shape[2]))(x)
    x = layers.Flatten()(x)
    x = layers.Dropout(dropout)(x)
    x = layers.Dense(len(cfg.LABELS))(x)
    out = layers.Softmax(name="probs")(x)
    return keras.Model(inp, out, name="ds_cnn_kws")
