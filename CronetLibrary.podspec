Pod::Spec.new do |s|
  s.name         = "CronetFramework"
  s.version      = "0.0.1"
  s.summary      = "Cronet.framework for Mac"
  s.description  = <<-DESC
                      Cronet.framework for Mac
                   DESC

  s.homepage     = "https://chromium.googlesource.com/chromium/src/+/lkgr/components/cronet"

  s.license      = { :type => "LGPL" }

  s.authors      = ['Chromium Authors']
  s.source       = { :git => "https://github.com/Naituw/Cronet-Mac.git"}

  s.platform = :osx, '10.10'
  s.frameworks = ['GSS', 'AppKit', 'SystemConfiguration', 'Foundation']
  s.libraries  = ['bsm', 'resolv', 'z', 'c++']

  library_path = ['libCronetNetworking.a']

  s.source_files = ['CronetNetworking/Cronet/components/cronet/ios/{Cronet,bidirectional_stream_c}.h']
  s.vendored_libraries = library_path
  s.user_target_xcconfig = { '__CronetAvailable__' => '1',
                             '__CronetIsFramework__' => '0'}

end