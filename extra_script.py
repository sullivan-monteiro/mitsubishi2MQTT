Import("env")

def after_upload(source, target, env):
    print("Démarrage du moniteur après le téléversement...")
    env.AutodetectUploadPort()
    env.RunTarget("monitor")

env.AddPostAction("upload", after_upload)
