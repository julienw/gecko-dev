Component.utils.import("resource://gre/modules/XPCOMUtils.jsm");

Components.utils.import("resource://gre/modules/Downloads.jsm");
Components.utils.import("resource://gre/modules/osfile.jsm");

XPCOMUtils.defineLazyModuleGetter(this, "DownloadIntegration",
                                  "resource://gre/modules/DownloadIntegration.jsm");

let lastError = null;

let currentId = 0;
const downloadsList = {};

function getDownload(downloadId) {
  return Promise.resolve().then(() => {
    var download = downloadsList[downloadID];
    if (!download) {
      throw new Error(`Bad downloadId '${downloadId}'`);
    }
    resolve(download);
  });
}

const downloads = Object.freeze({
  download(chromeOptions, callback) {
    Downloads.getPreferredDownloadsDirectory().then(downloadsDir => {
      const mozOptions = {
        source: chromeOptions.url,
        // TODO use file path to check and concatenate
        target: chromeOptions.target ?
          OS.Path.join(downloadsDir, chromeOptions.target):
          downloadsDir,
        // TODO do something with conflictAction,
        // TODO do something with saveAs,
      };

      return Downloads.createDownload(mozOptions);
    }).then(download => {
      downloadsList[++currentId] = { download };
      download.tryToKeepPartialData = true;
      downloadsList[currentId].currentPromise = download.start();
      return currentId;
    }).catch(e => {
      // TODO put something in runtime.lastError
      lastError = e;
      return undefined;
    }).then(callback);
  },

  search(query, callback) {
    throw new Error('not implemented');
  },

  pause(downloadId, callback) {
    getDownload(downloadId).then(downloadInfo => {
      return downloadInfo.download.cancel();
    }).catch((e) => {
      lastError = e;
    }).then(callback);
  },

  resume(downloadId, callback) {
    getDownload(downloadId).then(downloadInfo => {
      downloadInfo.promise = downloadInfo.download.start();
    }).catch(e => {
      lastError = e;
    }).then(callback);
  },

  cancel(downloadId, callback) {
    getDownload(downloadId).then(downloadInfo => {
      var returnPromise = downloadInfo.download.finalize();
      delete downloadsList[downloadId];
      return returnPromise;
    }).catch(e => {
      lastError = e;
    }).then(callback);
  },

  getFileIcon(downloadId, options, callback) {
    throw new Error('not implemented');
  },

  open(downloadId) {
    // TODO permission downloads.open
    getDownload(downloadId).then(downloadInfo => {
      if (!downloadInfo.download.succeeded) {
        throw new Error(`The download '${downloadId}' isn't complete yet`);
      }
      return downloadInfo.download.launch();
    }).catch(e => {
      lastError = e;
    });
  },

  show(downloadId) {
    getDownload(downloadId).then(downloadInfo => {
      return downloadInfo.download.showContainingDirectory();
    }).catch(e => {
      lastError = e;
    });
  },

  showDefaultFolder() {
    DownloadIntegration.showContainingDirectory(
      Downloads.getPreferredDownloadsDirectory()
    );
  },

  erase(query, callback) {
    throw new Error('not implemented');
  },

  removeFile(downloadId, callback) {
    getDownload(downloadId).then(downloadInfo => {
      if (!downloadInfo.download.succeeded) {
        throw new Error(`The download '${downloadId}' isn't complete yet`);
      }

      return OS.File.remove(
        downloadInfo.download.target.path,
        { ignoreAbsent: true }
      );
    }).catch(e => {
      lastError = e;
    }).then(callback);
  },
});

extensions.registerAPI((extension, context) => ({ downloads }));

