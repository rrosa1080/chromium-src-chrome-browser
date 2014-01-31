// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

'use strict';

/**
 * @param {HTMLElement} container Container element.
 * @constructor
 */
function AudioPlayer(container) {
  this.container_ = container;
  this.volumeManager_ = new VolumeManagerWrapper(
      VolumeManagerWrapper.DriveEnabledStatus.DRIVE_ENABLED);
  this.metadataCache_ = MetadataCache.createFull(this.volumeManager_);

  this.currentTrackIndex_ = -1;
  this.playlistGeneration_ = 0;

  /**
   * Whether if the playlist is expanded or not. This value is changed by
   * this.syncExpanded().
   * True: expanded, false: collapsed, null: unset.
   *
   * @type {?boolean}
   * @private
   */
  this.isExpanded_ = null;  // Initial value is null. It'll be set in load().

  this.player_ = document.querySelector('audio-player');
  this.trackListItems_ = [];
  this.player_.tracks = this.trackListItems_;
  Platform.performMicrotaskCheckpoint();

  this.errorString_ = '';
  this.offlineString_ = '';
  chrome.fileBrowserPrivate.getStrings(function(strings) {
    container.ownerDocument.title = strings['AUDIO_PLAYER_TITLE'];
    this.errorString_ = strings['AUDIO_ERROR'];
    this.offlineString_ = strings['AUDIO_OFFLINE'];
    AudioPlayer.TrackInfo.DEFAULT_ARTIST =
        strings['AUDIO_PLAYER_DEFAULT_ARTIST'];
  }.bind(this));

  this.volumeManager_.addEventListener('externally-unmounted',
      this.onExternallyUnmounted_.bind(this));

  window.addEventListener('resize', this.onResize_.bind(this));

  // Show the window after DOM is processed.
  var currentWindow = chrome.app.window.current();
  if (currentWindow)
    setTimeout(currentWindow.show.bind(currentWindow), 0);
}

/**
 * Initial load method (static).
 */
AudioPlayer.load = function() {
  document.ondragstart = function(e) { e.preventDefault() };

  // TODO(mtomasz): Consider providing an exact size icon, instead of relying
  // on downsampling by ash.
  chrome.app.window.current().setIcon(
      'audio_player/icons/audio-player-64.png');
  AudioPlayer.instance =
      new AudioPlayer(document.querySelector('.audio-player'));

  reload();
};

/**
 * Unloads the player.
 */
function unload() {
  if (AudioPlayer.instance)
    AudioPlayer.instance.onUnload();
}

/**
 * Reloads the player.
 */
function reload() {
  if (window.appState) {
    util.saveAppState();
    AudioPlayer.instance.load(window.appState);
    return;
  }
}

/**
 * Loads a new playlist.
 * @param {Playlist} playlist Playlist object passed via mediaPlayerPrivate.
 */
AudioPlayer.prototype.load = function(playlist) {
  this.playlistGeneration_++;
  this.currentTrackIndex_ = -1;

  // Save the app state, in case of restart.
  window.appState = playlist;
  util.saveAppState();

  this.syncExpanded();

  // Resolving entries has to be done after the volume manager is initialized.
  this.volumeManager_.ensureInitialized(function() {
    util.URLsToEntries(playlist.items, function(entries) {
      this.entries_ = entries;

      var position = playlist.position || 0;
      var time = playlist.time || 0;

      if (this.entries_.length == 0)
        return;

      this.trackListItems_.splice(0);

      for (var i = 0; i != this.entries_.length; i++) {
        var entry = this.entries_[i];
        var onClick = this.select_.bind(this, i, false /* no restore */);
        this.trackListItems_.push(new AudioPlayer.TrackInfo(entry, onClick));
      }

      this.select_(position, !!time);

      // Load the selected track metadata first, then load the rest.
      this.loadMetadata_(position);
      for (i = 0; i != this.entries_.length; i++) {
        if (i != position)
          this.loadMetadata_(i);
      }
    }.bind(this));
  }.bind(this));
};

/**
 * Loads metadata for a track.
 * @param {number} track Track number.
 * @private
 */
AudioPlayer.prototype.loadMetadata_ = function(track) {
  this.fetchMetadata_(
      this.entries_[track], this.displayMetadata_.bind(this, track));
};

/**
 * Displays track's metadata.
 * @param {number} track Track number.
 * @param {Object} metadata Metadata object.
 * @param {string=} opt_error Error message.
 * @private
 */
AudioPlayer.prototype.displayMetadata_ = function(track, metadata, opt_error) {
  this.trackListItems_[track].setMetadata(metadata, opt_error);
};

/**
 * Closes audio player when a volume containing the selected item is unmounted.
 * @param {Event} event The unmount event.
 * @private
 */
AudioPlayer.prototype.onExternallyUnmounted_ = function(event) {
  if (!this.selectedItemFilesystemPath_)
    return;
  if (this.selectedItemFilesystemPath_.indexOf(event.mountPath) == 0)
    close();
};

/**
 * Called on window is being unloaded.
 */
AudioPlayer.prototype.onUnload = function() {
  if (this.player_)
    this.player_.onPageUnload();

  if (this.volumeManager_)
    this.volumeManager_.dispose();
};

/**
 * Selects a new track to play.
 * @param {number} newTrack New track number.
 * @param {boolean=} opt_restoreState True if restoring the play state from URL.
 * @private
 */
AudioPlayer.prototype.select_ = function(newTrack, opt_restoreState) {
  if (this.currentTrackIndex_ == newTrack) return;

  this.currentTrackIndex_ = newTrack;
  this.player_.currentTrackIndex = this.currentTrackIndex_;

  if (window.appState) {
    window.appState.position = this.currentTrackIndex_;
    window.appState.time = 0;
    util.saveAppState();
  } else {
    util.platform.setPreference(AudioPlayer.TRACK_KEY, this.currentTrackIndex_);
  }

  var entry = this.entries_[this.currentTrackIndex_];

  this.fetchMetadata_(entry, function(metadata) {
    if (this.currentTrackIndex_ != newTrack)
      return;

    // Resolve real filesystem path of the current audio file.
    this.selectedItemFilesystemPath_ = null;
    this.selectedItemFilesystemPath_ = entry.fullPath;
  }.bind(this));
};

/**
 * @param {FileEntry} entry Track file entry.
 * @param {function(object)} callback Callback.
 * @private
 */
AudioPlayer.prototype.fetchMetadata_ = function(entry, callback) {
  this.metadataCache_.get(entry, 'thumbnail|media|streaming',
      function(generation, metadata) {
        // Do nothing if another load happened since the metadata request.
        if (this.playlistGeneration_ == generation)
          callback(metadata);
      }.bind(this, this.playlistGeneration_));
};

/**
 * Media error handler.
 * @private
 */
AudioPlayer.prototype.onError_ = function() {
  var track = this.currentTrackIndex_;

  this.invalidTracks_[track] = true;

  this.fetchMetadata_(
      this.entries_[track],
      function(metadata) {
        var error = (!navigator.onLine && metadata.streaming) ?
            this.offlineString_ : this.errorString_;
        this.displayMetadata_(track, metadata, error);
        this.scheduleAutoAdvance_();
      }.bind(this));
};

/**
 * Expands/collapses button click handler. Toggles the mode and updates the
 * height of the window.
 *
 * @private
 */
AudioPlayer.prototype.onExpandCollapse_ = function() {
  if (this.isExpanded_) {
    this.player_.expand(false);
  } else {
    this.player_.expand(true);
  }
  this.syncHeight_();
};

/**
 * Toggles the expanded mode when resizing.
 *
 * @param {Event} event Resize event.
 * @private
 */
AudioPlayer.prototype.onResize_ = function(event) {
  if (!this.isExpanded_ &&
      window.innerHeight >= AudioPlayer.EXPANDED_MODE_MIN_HEIGHT) {
    this.isExpanded_ = true;
    this.player_.expand(true);
  } else if (this.isExpanded_ &&
             window.innerHeight < AudioPlayer.EXPANDED_MODE_MIN_HEIGHT) {
    this.isExpanded_ = false;
    this.player_.expand(false);
  }
};

/* Keep the below constants in sync with the CSS. */

/**
 * Window header size in pixels.
 * @type {number}
 * @const
 */
AudioPlayer.HEADER_HEIGHT = 28;

/**
 * Track height in pixels.
 * @type {number}
 * @const
 */
AudioPlayer.TRACK_HEIGHT = 44;

/**
 * Controls bar height in pixels.
 * @type {number}
 * @const
 */
AudioPlayer.CONTROLS_HEIGHT = 72;

/**
 * Default number of items in the expanded mode.
 * @type {number}
 * @const
 */
AudioPlayer.DEFAULT_EXPANDED_ITEMS = 5;

/**
 * Minimum size of the window in the expanded mode in pixels.
 * @type {number}
 * @const
 */
AudioPlayer.EXPANDED_MODE_MIN_HEIGHT = AudioPlayer.CONTROLS_HEIGHT +
                                       AudioPlayer.TRACK_HEIGHT * 2;

/**
 * Sets the correct player window height.
 */
AudioPlayer.prototype.syncExpanded = function() {
  if (this.isExpanded_ !== null &&
      this.isExpanded_ == this.player_.isExpanded())
    return;

  if (this.isExpanded_ && !this.player_.isExpanded())
    this.lastExpandedHeight_ = window.innerHeight;

  this.isExpanded_ = this.player_.isExpanded();
  this.syncHeight_();
};

/**
 * @private
 */
AudioPlayer.prototype.syncHeight_ = function() {
  var targetHeight;

  if (this.isExpanded_) {
    // Expanded.
    if (!this.lastExpandedHeight_ ||
        this.lastExpandedHeight_ < AudioPlayer.EXPANDED_MODE_MIN_HEIGHT) {
      var expandedListHeight =
        Math.min(this.entries_.length, AudioPlayer.DEFAULT_EXPANDED_ITEMS) *
                                       AudioPlayer.TRACK_HEIGHT;
      targetHeight = AudioPlayer.CONTROLS_HEIGHT + expandedListHeight;
      this.lastExpandedHeight_ = targetHeight;
    } else {
      targetHeight = this.lastExpandedHeight_;
    }
  } else {
    // Not expaned.
    targetHeight = AudioPlayer.CONTROLS_HEIGHT + AudioPlayer.TRACK_HEIGHT;
  }

  window.resizeTo(window.innerWidth, targetHeight + AudioPlayer.HEADER_HEIGHT);
};

/**
 * Create a TrackInfo object encapsulating the information about one track.
 *
 * @param {fileEntry} entry FileEntry to be retrieved the track info from.
 * @param {function} onClick Click handler.
 * @constructor
 */
AudioPlayer.TrackInfo = function(entry, onClick) {
  this.url = entry.toURL();
  this.title = entry.name;
  this.artist = this.getDefaultArtist();

  // TODO(yoshiki): implement artwork.
  this.artwork = null;
  this.active = false;
};

/**
 * @return {HTMLDivElement} The wrapper element for the track.
 */
AudioPlayer.TrackInfo.prototype.getBox = function() { return this.box_ };

/**
 * @return {string} Default track title (file name extracted from the url).
 */
AudioPlayer.TrackInfo.prototype.getDefaultTitle = function() {
  var title = this.url.split('/').pop();
  var dotIndex = title.lastIndexOf('.');
  if (dotIndex >= 0) title = title.substr(0, dotIndex);
  title = decodeURIComponent(title);
  return title;
};

/**
 * TODO(kaznacheev): Localize.
 */
AudioPlayer.TrackInfo.DEFAULT_ARTIST = 'Unknown Artist';

/**
 * @return {string} 'Unknown artist' string.
 */
AudioPlayer.TrackInfo.prototype.getDefaultArtist = function() {
  return AudioPlayer.TrackInfo.DEFAULT_ARTIST;
};

/**
 * @param {Object} metadata The metadata object.
 * @param {string} error Error string.
 */
AudioPlayer.TrackInfo.prototype.setMetadata = function(
    metadata, error) {
  if (error) {
    // TODO(yoshiki): Handle error in better way.
    this.title = entry.name;
    this.artist = this.getDefaultArtist();
  } else if (metadata.thumbnail && metadata.thumbnail.url) {
    // TODO(yoshiki): implement artwork.
  }
  this.title = (metadata.media && metadata.media.title) ||
      this.getDefaultTitle();
  this.artist = error ||
      (metadata.media && metadata.media.artist) || this.getDefaultArtist();
};

// Starts loading the audio player.
window.addEventListener('WebComponentsReady', function(e) {
  AudioPlayer.load();
});