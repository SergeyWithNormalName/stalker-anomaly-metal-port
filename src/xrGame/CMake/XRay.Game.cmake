add_module(XRay.Game
  TYPE STATIC
  
  INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}
  
  DEFINES
  GP_EMAIL_LEN=128
  GP_NICK_LEN=32
  GP_UNIQUENICK_LEN=32
  GP_PASSWORD_LEN=32
  XRGAME_EXPORTS
  
  LINKS
  DPlay
  CxImage
  IKAN

  XRay.Core.Defines
  XRay.Engine.Defines

  XRay.Includes
  XRay.Collision.Includes
  XRay.Core.Includes
  XRay.Core.Crypto.Includes
  XRay.Engine.Includes
  XRay.NetServer.Includes
  XRay.Physics.Includes
  XRay.Render.API.Includes
  XRay.ServerEntities.Includes

  SOURCES
  xrGame.cpp
)

include(XRay.Game.AI)
include(XRay.Game.Core)
include(XRay.Game.Physics)
include(XRay.Game.UI)