"""Import the source loop as a native, gapless PCM Sound Wave, or audit its saved settings."""

from pathlib import Path
import wave

import unreal


AUDIO_DIR = '/Game/LunaArt/Audio'
ASSET_NAME = 'SW_Luna_QuietMeridian'
PACKAGE_NAME = f'{AUDIO_DIR}/{ASSET_NAME}'
SOURCE = Path(unreal.Paths.project_dir()) / 'SourceArt' / 'BGM' / f'{ASSET_NAME}.wav'


def import_loop():
    with wave.open(str(SOURCE), 'rb') as wav:
        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (2, 2, 48000)
        assert wav.getnframes() > 30 * wav.getframerate()

    task = unreal.AssetImportTask()
    task.set_editor_property('filename', str(SOURCE.resolve()))
    task.set_editor_property('destination_path', AUDIO_DIR)
    task.set_editor_property('destination_name', ASSET_NAME)
    task.set_editor_property('automated', True)
    task.set_editor_property('replace_existing', True)
    task.set_editor_property('save', False)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    assert task.get_editor_property('imported_object_paths'), SOURCE

    asset = unreal.EditorAssetLibrary.load_asset(PACKAGE_NAME)
    assert isinstance(asset, unreal.SoundWave), PACKAGE_NAME
    asset.set_editor_property('looping', True)
    asset.set_editor_property('loading_behavior', unreal.SoundWaveLoadingBehavior.FORCE_INLINE)
    asset.set_sound_asset_compression_type(unreal.SoundAssetCompressionType.PCM)
    assert unreal.EditorAssetLibrary.save_loaded_asset(asset, False), PACKAGE_NAME


def audit_loop():
    asset = unreal.EditorAssetLibrary.load_asset(PACKAGE_NAME)
    assert isinstance(asset, unreal.SoundWave), PACKAGE_NAME
    assert asset.get_editor_property('looping')
    assert asset.get_editor_property('loading_behavior') == unreal.SoundWaveLoadingBehavior.FORCE_INLINE
    assert asset.get_sound_asset_compression_type() == unreal.SoundAssetCompressionType.PCM
    assert asset.get_editor_property('num_channels') == 2
    sample_rate = asset.get_editor_property('imported_sample_rate')
    assert sample_rate == 48000, sample_rate
    duration = asset.get_editor_property('duration')
    assert duration > 30.0, duration
    unreal.log(f'LUNA_BGM_ASSET path={asset.get_path_name()} duration={duration:.3f} sample_rate={sample_rate} channels=2 codec=PCM loading=ForceInline looping=1')


if '-LunaBgmAudit' not in unreal.SystemLibrary.get_command_line():
    import_loop()
audit_loop()
