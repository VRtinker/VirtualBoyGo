#include "VBEmulator.h"
#include <VrApi/Include/VrApi_Types.h>
#include <VrAppFramework/Src/Framebuffer.h>
#include <sys/stat.h>
#include <vrvb.h>
#include <fstream>
#include <sstream>
#include "Audio/OpenSLWrap.h"
#include "DrawHelper.h"
#include "FontMaster.h"
#include "Global.h"
#include "LayerBuilder.h"
#include "MenuHelper.h"

template<typename T>
std::string to_string(T value) {
  std::ostringstream os;
  os << value;
  return os.str();
}

template<>
void MenuList<VBEmulator::Rom>::DrawTexture(float offsetX, float offsetY, float transparency) {
  // calculate the slider position
  float scale = maxListItems / (float) ItemList->size();
  if (scale > 1) scale = 1;
  GLfloat recHeight = scrollbarHeight * scale;

  GLfloat sliderPercentage = 0;
  if (ItemList->size() > maxListItems)
    sliderPercentage = (menuListState / (float) (ItemList->size() - maxListItems));
  else
    sliderPercentage = 0;

  GLfloat recPosY = (scrollbarHeight - recHeight) * sliderPercentage;

  // slider background
  DrawHelper::DrawTexture(textureWhiteId, PosX + offsetX + 2, PosY + 2, scrollbarWidth - 4,
                          scrollbarHeight - 4, MenuBackgroundOverlayColor, transparency);
  // slider
  DrawHelper::DrawTexture(textureWhiteId, PosX + offsetX, PosY + recPosY, scrollbarWidth, recHeight,
                          sliderColor, transparency);

  // draw the cartridge icons
  for (uint i = (uint) menuListFState; i < menuListFState + maxListItems; i++) {
    if (i < ItemList->size()) {
      // fading in or out
      float fadeTransparency = 1;
      if (i - menuListFState < 0) {
        fadeTransparency = 1 - (menuListFState - i);
      } else if (i - menuListFState >= maxListItems - 1 && menuListFState != (int) menuListFState) {
        fadeTransparency = menuListFState - (int) menuListFState;
      }

      DrawHelper::DrawTexture(textureVbIconId,
                              PosX + offsetX + scrollbarWidth + 15
                                  + (((uint) CurrentSelection == i) ? 5 : 0),
                              listStartY + listItemSize / 2 - 12
                                  + listItemSize * (i - menuListFState) + offsetY, 21, 24,
                              {1.0f, 1.0f, 1.0f, 1.0f}, transparency * fadeTransparency);
    }
  }
}

template<>
void MenuList<VBEmulator::Rom>::DrawText(float offsetX, float offsetY, float transparency) {
  // draw rom list
  for (uint i = (uint) menuListFState; i < menuListFState + maxListItems; i++) {
    if (i < ItemList->size()) {
      // fading in or out
      float fadeTransparency = 1;
      if (i - menuListFState < 0) {
        fadeTransparency = 1 - (menuListFState - i);
      } else if (i - menuListFState >= maxListItems - 1 && menuListFState != (int) menuListFState) {
        fadeTransparency = menuListFState - (int) menuListFState;
      }

      FontManager::RenderText(
          *Font,
          ItemList->at(i).RomName,
          PosX + offsetX + scrollbarWidth + 44 + (((uint) CurrentSelection == i) ? 5 : 0),
          listStartY + itemOffsetY + listItemSize * (i - menuListFState) + offsetY,
          1.0f,
          ((uint) CurrentSelection == i) ? textSelectionColor : textColor,
          transparency * fadeTransparency);
    } else
      break;
  }
}

namespace VBEmulator {

GLuint screenTextureId, stateImageId;
GLuint screenTextureCylinderId;
ovrTextureSwapChain *CylinderSwapChain;

ovrSurfaceDef SurfaceDef;
GlProgram Program;

static const char VERTEX_SHADER[] =
    "in vec3 Position;\n"
        "in vec4 VertexColor;\n"
        "in vec2 TextureCoords;\n"

        "in mat4 VertexTransform;\n"

        "out vec4 fragmentColor;\n"
        //"out vec2 texCoords;\n"

        "void main()\n"
        "{\n"
        "	gl_Position = sm.ProjectionMatrix[VIEW_ID] * ( sm.ViewMatrix[VIEW_ID] * ( VertexTransform "
        "* vec4( Position, 1.0 ) ) );\n"
        "	fragmentColor = VertexColor;\n"
        //"	texCoords = TextureCoords;\n"
        "}\n";

static const char FRAGMENT_SHADER[] =
    "in vec4 fragmentColor;\n"
        //"in vec2 texCoords;\n"

        "out vec4 color;\n"

        "uniform sampler2D text;\n"
        //"uniform vec4 textColor;\n"

        "void main()\n"
        "{\n"
        "	vec4 tex_sample = texture(text, vec2(fragmentColor.x, fragmentColor.y));\n"
        "	color = tex_sample * 0.85f + fragmentColor * 0.15f;// * tex_sample.a;\n"
        //"	color = fragmentColor;\n"
        "}\n";

static const char FRAGMENT_SHADER_TEXTURE[] =
    "#version 330 core\n"
        "in vec2 TexCoords;\n"

        "out vec4 color;\n"

        "uniform sampler2D text;\n"
        "uniform vec4 textColor;\n"

        "void main()\n"
        "{\n"
        "	vec4 tex_sample = texture(text, TexCoords);\n"
        "	color = tex_sample * textColor * tex_sample.a;\n"
        "}\n";

static const char *movieUiVertexShaderSrc =
    "uniform TextureMatrices\n"
        "{\n"
        "highp mat4 Texm[NUM_VIEWS];\n"
        "};\n"
        "attribute vec4 Position;\n"
        "attribute vec2 TexCoord;\n"
        "uniform lowp vec4 UniformColor;\n"
        "varying  highp vec2 oTexCoord;\n"
        "varying  lowp vec4 oColor;\n"
        "void main()\n"
        "{\n"
        "   gl_Position = TransformVertex( Position );\n"
        "   oTexCoord = vec2( Texm[ VIEW_ID ] * vec4(TexCoord,1,1) );\n"
        "   oColor = UniformColor;\n"
        "}\n";

const char *movieUiFragmentShaderSrc =
    "uniform sampler2D Texture0;\n"
        "uniform sampler2D Texture1;\n"    // fade / clamp texture
        "uniform lowp vec4 ColorBias;\n"
        "varying highp vec2 oTexCoord;\n"
        "varying lowp vec4	oColor;\n"
        "void main()\n"
        "{\n"
        "	lowp vec4 movieColor = texture2D( Texture0, oTexCoord ) * texture2D( Texture1, oTexCoord );\n"
        "	gl_FragColor = ColorBias + oColor * movieColor;\n"
        "}\n";

static const int NUM_INSTANCES = 150;
static const int NUM_ROTATIONS = 16;

GlGeometry Cube;
ovrVector3f CubePositions[NUM_INSTANCES];

GLint VertexTransformAttribute;
GLuint InstanceTransformBuffer;

// setup Cube
struct ovrCubeVertices {
  Vector3f positions[8];
  Vector4f colors[8];
  Vector2f texcoords[8];
};

int CubeRotations[NUM_INSTANCES];
ovrVector3f Rotations[NUM_ROTATIONS];

const void *currentScreenData = nullptr;

static ovrCubeVertices cubeVertices = {
    // positions
    {
        Vector3f(-1.0f, +1.0f, -1.0f), Vector3f(+1.0f, +1.0f, -1.0f), Vector3f(+1.0f, +1.0f, +1.0f),
        Vector3f(-1.0f, +1.0f, +1.0f),  // top
        Vector3f(-1.0f, -1.0f, -1.0f), Vector3f(-1.0f, -1.0f, +1.0f), Vector3f(+1.0f, -1.0f, +1.0f),
        Vector3f(+1.0f, -1.0f, -1.0f)  // bottom
    },
    // colors
    {
        Vector4f(1.0f, 0.0f, 1.0f, 1.0f), Vector4f(0.0f, 1.0f, 1.0f, 1.0f),
        Vector4f(1.0f, 1.0f, 0.0f, 1.0f), Vector4f(0.0f, 0.0f, 1.0f, 1.0f),
        Vector4f(0.0f, 0.0f, 1.0f, 1.0f), Vector4f(0.0f, 1.0f, 0.0f, 1.0f),
        Vector4f(1.0f, 0.0f, 1.0f, 1.0f), Vector4f(1.0f, 0.0f, 0.0f, 1.0f)
    },
    {
        Vector2f(1.0f, 0.0f), Vector2f(0.0f, 1.0f), Vector2f(1.0f, 0.0f), Vector2f(1.0f, 1.0f),
        Vector2f(0.0f, 1.0f), Vector2f(0.0f, 1.0f), Vector2f(1.0f, 0.0f), Vector2f(1.0f, 1.0f)
    }
};

static const unsigned short cubeIndices[36] = {
    0, 2, 1, 2, 0, 3,  // top
    4, 6, 5, 6, 4, 7,  // bottom
    2, 6, 7, 7, 1, 2,  // right
    0, 4, 5, 5, 3, 0,  // left
    3, 5, 6, 6, 2, 3,  // front
    0, 1, 7, 7, 4, 0   // back
};

ovrSurfaceDef ScreenSurfaceDef;
Bounds3f SceneScreenBounds;

Vector4f ScreenColor[2];        // { UniformColor, ScaleBias }
GlTexture ScreenTexture[2];    // { MovieTexture, Fade Texture }
Matrix4f ScreenTexMatrix[2];
GlBuffer ScreenTexMatrices;

double startTime;

const float COLOR_STEP_SIZE = 0.05f;

// 384
// 768
const int VIDEO_WIDTH = 384;
const int VIDEO_HEIGHT = 224;

const int CylinderWidth = VIDEO_WIDTH;
const int CylinderHeight = VIDEO_HEIGHT;

std::string strColor[]{"R: ", "G: ", "B: "};
float color[]{1.0f, 1.0f, 1.0f};

int selectedPredefColor;
const int predefColorCount = 11;
ovrVector3f predefColors[] = {{1.0f, 0.0f, 0.0f}, {0.9f, 0.3f, 0.1f}, {1.0f, 0.85f, 0.1f},
                              {0.25f, 1.0f, 0.1f}, {0.0f, 1.0f, 0.45f}, {0.0f, 1.0f, 0.85f},
                              {0.0f, 0.85f, 1.0f}, {0.15f, 1.0f, 1.0f}, {0.75f, 0.65f, 1.0f},
                              {1.0f, 1.0f, 1.0f}, {1.0f, 0.3f, 0.2f}};

/*
 *     {BUTTON_A, BUTTON_B, BUTTON_RIGHT_TRIGGER, BUTTON_LEFT_TRIGGER, BUTTON_RSTICK_UP,
     BUTTON_RSTICK_RIGHT, BUTTON_LSTICK_RIGHT, BUTTON_LSTICK_LEFT,
     BUTTON_LSTICK_DOWN, BUTTON_LSTICK_UP, BUTTON_START, BUTTON_SELECT,
     BUTTON_RSTICK_LEFT, BUTTON_RSTICK_DOWN};
 */

//const uint buttonCount = 14;
GLuint *button_icons[buttonCount] =
    {&textureButtonAIconId, &textureButtonBIconId, &mappingTriggerRight, &mappingTriggerLeft,
     &mappingRightUpId, &mappingRightRightId, &mappingLeftRightId, &mappingLeftLeftId,
     &mappingLeftDownId, &mappingLeftUpId, &mappingStartId, &mappingSelectId, &mappingRightLeftId,
     &mappingRightDownId};
uint button_mapping_index[buttonCount] = {0, 1, 8, 9, 18, 21, 17, 16, 15, 14, 4, 6, 20, 19};
uint button_mapping[buttonCount];

LoadedGame *currentGame;

const std::string romFolderPath = "/Roms/VB/";
std::string stateFolderPath;

const std::vector<std::string> supportedFileNames = {".vb", ".vboy", ".bin"};

MenuList<Rom> *romList;
std::vector<Rom> *romFileList = new std::vector<Rom>();

bool audioInit;

float emulationSpeed = 50.27;
float frameCounter = 1;

uint8_t *screenData;

int screenPosY;

int TextureHeight = VIDEO_HEIGHT * 2 + 12;

int32_t *pixelData = new int32_t[VIDEO_WIDTH * TextureHeight];

int32_t *stateImageData = new int32_t[VIDEO_WIDTH * VIDEO_HEIGHT];

bool useCubeMap = false;
bool useThreeDeeMode = true;

Rom *CurrentRom;
GLuint screenFramebuffer[2];
int romSelection;

MenuButton *rButton, *gButton, *bButton;

void LoadRam();

void InitStateImage() {
  glGenTextures(1, &stateImageId);
  glBindTexture(GL_TEXTURE_2D, stateImageId);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, VIDEO_WIDTH, VIDEO_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);
}

void UpdateStateImage(int saveSlot) {
  glBindTexture(GL_TEXTURE_2D, stateImageId);

  uint8_t *dataArray = currentGame->saveStates[saveSlot].saveImage;
  for (int y = 0; y < VIDEO_HEIGHT; ++y) {
    for (int x = 0; x < VIDEO_WIDTH; ++x) {
      uint8_t das = dataArray[x + y * VIDEO_WIDTH];
      stateImageData[x + y * VIDEO_WIDTH] = 0xFF000000 | ((int) (das * color[2]) << 16) |
          ((int) (das * color[1]) << 8) | (int) (das * color[0]);
    }
  }

  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VIDEO_WIDTH, VIDEO_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE,
                  stateImageData);
  glBindTexture(GL_TEXTURE_2D, 0);
}

void UpdateScreen(const void *data) {
  screenData = (uint8_t *) data;
  uint8_t *dataArray = (uint8_t *) data;

  {
    for (int y = 0; y < TextureHeight; ++y) {
      for (int x = 0; x < VIDEO_WIDTH; ++x) {
        uint8_t das = dataArray[x + y * VIDEO_WIDTH];
        // uint8_t das = dataArray[x / 2 + y / 2 * VIDEO_WIDTH * 2 + VIDEO_WIDTH];
        pixelData[x + y * VIDEO_WIDTH] = 0xFF000000 | ((int) (das * color[2]) << 16) |
            ((int) (das * color[1]) << 8) | (int) (das * color[0]);

      }
    }

    // make the space between the two images transparent
    memset(&pixelData[VIDEO_WIDTH * VIDEO_HEIGHT], 0, 12 * VIDEO_WIDTH * 4);

    glBindTexture(GL_TEXTURE_2D, screenTextureId);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, CylinderWidth, TextureHeight, GL_RGBA,
                    GL_UNSIGNED_BYTE, pixelData);
    glBindTexture(GL_TEXTURE_2D, 0);

    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
    glBlendEquation(GL_FUNC_ADD);
    // render image to the screen texture
    glBindFramebuffer(GL_FRAMEBUFFER, screenFramebuffer[0]);
    glViewport(0, 0, VIDEO_WIDTH * 2 + 24, TextureHeight * 2 + 24);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    // @HACK: make DrawTexture better
    // 640, 576 is used because it is the full size of the projection set before
    // TODO use 6px border
    DrawHelper::DrawTexture(screenTextureId,
                            640 * (12.0f / (VIDEO_WIDTH * 2 + 24)),
                            576 * (12.0f / (TextureHeight * 2 + 24)),
                            640 * ((float) (VIDEO_WIDTH * 2) / (VIDEO_WIDTH * 2 + 24)),
                            576 * ((float) (TextureHeight * 2) / (TextureHeight * 2 + 24)),
                            {1.0f, 1.0f, 1.0f, 1.0f},
                            1);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }

  ScreenTexture[0] =
      GlTexture(screenTextureCylinderId,
                GL_TEXTURE_2D, VIDEO_WIDTH * 2 + 24, TextureHeight * 2 + 24);
  ScreenTexture[1] =
      GlTexture(screenTextureCylinderId,
                GL_TEXTURE_2D, VIDEO_WIDTH * 2 + 24, TextureHeight * 2 + 24);

}

void AudioFrame(unsigned short *audio, int32_t sampleCount) {
  if (!audioInit) {
    audioInit = true;
    StartPlaying();
  }

  SetBuffer(audio, (unsigned) (sampleCount * 2));
  // 52602
  // 877
  // LOG("VRVB audio size: %i", sampleCount);
}

void VB_Audio_CB(int16_t *SoundBuf, int32_t SoundBufSize) {
  AudioFrame((unsigned short *) SoundBuf, SoundBufSize);
}

void VB_VIDEO_CB(const void *data, unsigned width, unsigned height) {
  // LOG("VRVB width: %i, height: %i, %i", width, height, (((int8_t *) data)[5])); // 144 + 31 * 384
  // update the screen texture with the newly received image
  currentScreenData = data;
  UpdateScreen(data);
}

bool StateExists(int slot) {
  std::string savePath = stateFolderPath + CurrentRom->RomName + ".state";
  if (slot > 0) savePath += to_string(slot);
  struct stat buffer;
  return (stat(savePath.c_str(), &buffer) == 0);
}

void SaveStateImage(int slot) {
  std::string savePath = stateFolderPath + CurrentRom->RomName + ".stateimg";
  if (slot > 0) savePath += to_string(slot);

  LOG("save image of slot to %s", savePath.c_str());
  std::ofstream outfile(savePath, std::ios::trunc | std::ios::binary);
  outfile.write((const char *) currentGame->saveStates[slot].saveImage,
                sizeof(uint8_t) * VIDEO_WIDTH * VIDEO_HEIGHT);
  outfile.close();
  LOG("finished writing save image to file");
}

bool LoadStateImage(int slot) {
  std::string savePath = stateFolderPath + CurrentRom->RomName + ".stateimg";
  if (slot > 0) savePath += to_string(slot);

  std::ifstream file(savePath, std::ios::in | std::ios::binary | std::ios::ate);
  if (file.is_open()) {
    uint8_t *data = new uint8_t[VIDEO_WIDTH * VIDEO_HEIGHT];
    file.seekg(0, std::ios::beg);
    file.read((char *) data, sizeof(uint8_t) * VIDEO_WIDTH * VIDEO_HEIGHT);
    file.close();

    memcpy(currentGame->saveStates[slot].saveImage, data,
           sizeof(uint8_t) * VIDEO_WIDTH * VIDEO_HEIGHT);

    delete[] data;

    LOG("loaded image file: %s", savePath.c_str());

    return true;
  }

  LOG("could not load image file: %s", savePath.c_str());
  return false;
}

void LoadGame(Rom *rom) {
  // save the ram of the old rom
  SaveRam();

  LOG("LOAD VRVB ROM %s", rom->FullPath.c_str());
  std::ifstream file(rom->FullPath, std::ios::in | std::ios::binary | std::ios::ate);
  if (file.is_open()) {
    long romBufferSize = file.tellg();
    char *memblock = new char[romBufferSize];

    file.seekg(0, std::ios::beg);
    file.read(memblock, romBufferSize);
    file.close();

    VRVB::LoadRom((const uint8_t *) memblock, (size_t) romBufferSize);

    delete[] memblock;

    CurrentRom = rom;
    LOG("finished loading rom %ld", romBufferSize);

    LOG("start loading ram");
    LoadRam();
    LOG("finished loading ram");
  } else {
    LOG("could not load VB rom file");
  }

  for (int i = 0; i < 10; ++i) {
    if (!LoadStateImage(i)) {
      currentGame->saveStates[i].hasImage = false;

      // clear memory
      memset(currentGame->saveStates[i].saveImage, 0,
             sizeof(uint8_t) * VIDEO_WIDTH * 2 * VIDEO_HEIGHT);
    } else {
      currentGame->saveStates[i].hasImage = true;
    }

    currentGame->saveStates[i].hasState = StateExists(i);
  }

  UpdateStateImage(0);

  LOG("LOADED VRVB ROM");
}

void Init(std::string appFolderPath) {
  stateFolderPath = appFolderPath + "/Roms/VB/States/";

  LOG("VRVB INIT w %i, %i, %i, %i", CylinderWidth, CylinderHeight, VIDEO_WIDTH, VIDEO_HEIGHT);
  // emu screen layer
  // left layer
  int cubeSizeX = CylinderWidth;
  int cubeSizeY = CylinderWidth;

  screenPosY = CylinderWidth / 2 - CylinderHeight / 2;
  LOG("screePosY %i", screenPosY);

  for (int y = 0; y < cubeSizeY; ++y) {
    for (int x = 0; x < cubeSizeX; ++x) {
      pixelData[x + y * cubeSizeX] = 0xFFFF00FF;
    }
  }
  GLfloat borderColor[] = {1.0f, 0.0f, 0.0f, 1.0f};

  glGenTextures(1, &screenTextureId);
  glBindTexture(GL_TEXTURE_2D, screenTextureId);
  glTexImage2D(GL_TEXTURE_2D,
               0,
               GL_RGBA,
               VIDEO_WIDTH,
               VIDEO_HEIGHT * 2 + 12,
               0,
               GL_RGBA,
               GL_UNSIGNED_BYTE,
               NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
  glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
  glBindTexture(GL_TEXTURE_2D, 0);

  {
    int borderSize = 24;
    // left texture
    CylinderSwapChain =
        vrapi_CreateTextureSwapChain(VRAPI_TEXTURE_TYPE_2D,
                                     VRAPI_TEXTURE_FORMAT_8888_sRGB,
                                     CylinderWidth * 2 + borderSize,
                                     TextureHeight * 2 + borderSize,
                                     1,
                                     false);
    screenTextureCylinderId = vrapi_GetTextureSwapChainHandle(CylinderSwapChain, 0);
    glBindTexture(GL_TEXTURE_2D, screenTextureCylinderId);
    glTexSubImage2D(GL_TEXTURE_2D,
                    0,
                    0,
                    0,
                    CylinderWidth * 2 + borderSize,
                    TextureHeight * 2 + borderSize,
                    GL_RGBA,
                    GL_UNSIGNED_BYTE,
                    NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    //glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
    glBindTexture(GL_TEXTURE_2D, 0);

    // create the framebuffer for the screen texture
    glGenFramebuffers(1, &screenFramebuffer[0]);
    glBindFramebuffer(GL_FRAMEBUFFER, screenFramebuffer[0]);
    // Set "renderedTexture" as our colour attachement #0
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           screenTextureCylinderId, 0);
    // Set the list of draw buffers.
    GLenum DrawBuffers[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, DrawBuffers);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }

  LOG("INIT VRVB");
  VRVB::Init();

  VRVB::audio_cb = VB_Audio_CB;
  VRVB::video_cb = VB_VIDEO_CB;

  InitStateImage();
  currentGame = new LoadedGame();
  for (int i = 0; i < 10; ++i) {
    currentGame->saveStates[i].saveImage = new uint8_t[VIDEO_WIDTH * 2 * VIDEO_HEIGHT]();
  }

  static ovrProgramParm MovieExternalUiUniformParms[] =
      {
          {"TextureMatrices", ovrProgramParmType::BUFFER_UNIFORM},
          {"UniformColor", ovrProgramParmType::FLOAT_VECTOR4},
          {"Texture0", ovrProgramParmType::TEXTURE_SAMPLED},
          {"Texture1", ovrProgramParmType::TEXTURE_SAMPLED},
          {"ColorBias", ovrProgramParmType::FLOAT_VECTOR4},
      };
  GlProgram
      MovieExternalUiProgram = GlProgram::Build(movieUiVertexShaderSrc,
                                                movieUiFragmentShaderSrc,
                                                MovieExternalUiUniformParms,
                                                sizeof(MovieExternalUiUniformParms)
                                                    / sizeof(ovrProgramParm));

  ScreenTexMatrices.Create(GLBUFFER_TYPE_UNIFORM, sizeof(Matrix4f) * GlProgram::MAX_VIEWS, NULL);

  ScreenColor[0] = Vector4f(1.0f, 1.0f, 1.0f, 0.0f);
  ScreenColor[1] = Vector4f(0.0f, 0.0f, 0.0f, 0.0f);

  ScreenSurfaceDef.surfaceName = "ScreenSurf";
  ScreenSurfaceDef.geo = BuildTesselatedQuad(1, 1);
  ScreenSurfaceDef.graphicsCommand.Program = MovieExternalUiProgram;
  ScreenSurfaceDef.graphicsCommand.UniformData[0].Data = &ScreenTexMatrices;
  ScreenSurfaceDef.graphicsCommand.UniformData[1].Data = &ScreenColor[0];
  ScreenSurfaceDef.graphicsCommand.UniformData[2].Data = &ScreenTexture[0];
  ScreenSurfaceDef.graphicsCommand.UniformData[3].Data = &ScreenTexture[1];
  ScreenSurfaceDef.graphicsCommand.UniformData[4].Data = &ScreenColor[1];

  Vector3f size(5.25f, 5.25f * (VIDEO_HEIGHT / (float) VIDEO_WIDTH), 0.0f);

  SceneScreenBounds = Bounds3f(size * -0.5f, size * 0.5f);
  SceneScreenBounds.Translate(Vector3f(0.0f, 1.66f, -5.61f));

  startTime = SystemClock::GetTimeInSeconds();
}

void UpdateEmptySlotLabel(MenuItem *item, uint &buttonState, uint &lastButtonState) {
  item->Visible = !currentGame->saveStates[saveSlot].hasState;
}

void UpdateNoImageSlotLabel(MenuItem *item, uint &buttonState, uint &lastButtonState) {
  item->Visible =
      currentGame->saveStates[saveSlot].hasState && !currentGame->saveStates[saveSlot].hasImage;
}

void InitMainMenu(int posX, int posY, Menu &mainMenu) {
  // main menu
  mainMenu.MenuItems.push_back(new MenuImage(textureWhiteId, MENU_WIDTH - VIDEO_WIDTH - 20 - 5,
                                             HEADER_HEIGHT + 20 - 5, VIDEO_WIDTH + 10,
                                             VIDEO_HEIGHT + 10, MenuBackgroundOverlayColor));
  MenuLabel *emptySlotLabel =
      new MenuLabel(&fontSlot, "- Empty Slot -", MENU_WIDTH - VIDEO_WIDTH - 20, HEADER_HEIGHT + 20,
                    VIDEO_WIDTH, VIDEO_HEIGHT, {1.0f, 1.0f, 1.0f, 1.0f});
  emptySlotLabel->UpdateFunction = UpdateEmptySlotLabel;

  MenuLabel *noImageSlotLabel =
      new MenuLabel(&fontSlot, "- -", MENU_WIDTH - VIDEO_WIDTH - 20, HEADER_HEIGHT + 20,
                    VIDEO_WIDTH, VIDEO_HEIGHT, {1.0f, 1.0f, 1.0f, 1.0f});
  noImageSlotLabel->UpdateFunction = UpdateNoImageSlotLabel;

  mainMenu.MenuItems.push_back(emptySlotLabel);
  mainMenu.MenuItems.push_back(noImageSlotLabel);
  // image slot
  mainMenu.MenuItems.push_back(new MenuImage(stateImageId, MENU_WIDTH - VIDEO_WIDTH - 20,
                                             HEADER_HEIGHT + 20, VIDEO_WIDTH, VIDEO_HEIGHT,
                                             {1.0f, 1.0f, 1.0f, 1.0f}));
}

void ChangeColor(MenuButton *item, int colorIndex, float dir) {
  color[colorIndex] += dir;

  if (color[colorIndex] < 0)
    color[colorIndex] = 0;
  else if (color[colorIndex] > 1)
    color[colorIndex] = 1;

  item->Text = strColor[colorIndex] + to_string(color[colorIndex]);

  // update screen
  if (currentScreenData)
    UpdateScreen(currentScreenData);
  // update save slot color
  UpdateStateImage(saveSlot);
}

void ChangePalette(MenuButton *item, float dir) {
  selectedPredefColor += dir;
  if (selectedPredefColor < 0)
    selectedPredefColor = predefColorCount - 1;
  else if (selectedPredefColor >= predefColorCount)
    selectedPredefColor = 0;

  color[0] = predefColors[selectedPredefColor].x;
  color[1] = predefColors[selectedPredefColor].y;
  color[2] = predefColors[selectedPredefColor].z;

  ChangeColor(rButton, 0, 0);
  ChangeColor(gButton, 1, 0);
  ChangeColor(bButton, 2, 0);

  item->Text = "Palette: " + to_string(selectedPredefColor);

  // update screen
  if (currentScreenData)
    UpdateScreen(currentScreenData);
  // update save slot color
  UpdateStateImage(saveSlot);
}

void SetThreeDeeMode(MenuItem *item, bool newMode) {
  useThreeDeeMode = newMode;
  ((MenuButton *) item)->IconId = useThreeDeeMode ? threedeeIconId : twodeeIconId;
  ((MenuButton *) item)->Text = useThreeDeeMode ? "3D Screen" : "2D Screen";
}

void SetCurvedMove(MenuItem *item, bool newMode) {
  useCubeMap = newMode;
  ((MenuButton *) item)->Text = useCubeMap ? "Flat Screen" : "Curved Screen";
}

void OnClickCurveScreen(MenuItem *item) { SetCurvedMove(item, !useCubeMap); }
void OnClickScreenMode(MenuItem *item) { SetThreeDeeMode(item, !useThreeDeeMode); }
void OnClickPrefabColorLeft(MenuItem *item) { ChangePalette((MenuButton *) item, -1); }
void OnClickPrefabColorRight(MenuItem *item) { ChangePalette((MenuButton *) item, 1); }
void OnClickRLeft(MenuItem *item) { ChangeColor((MenuButton *) item, 0, -COLOR_STEP_SIZE); }
void OnClickRRight(MenuItem *item) { ChangeColor((MenuButton *) item, 0, COLOR_STEP_SIZE); }
void OnClickGLeft(MenuItem *item) { ChangeColor((MenuButton *) item, 1, -COLOR_STEP_SIZE); }
void OnClickGRight(MenuItem *item) { ChangeColor((MenuButton *) item, 1, COLOR_STEP_SIZE); }
void OnClickBLeft(MenuItem *item) { ChangeColor((MenuButton *) item, 2, -COLOR_STEP_SIZE); }
void OnClickBRight(MenuItem *item) { ChangeColor((MenuButton *) item, 2, COLOR_STEP_SIZE); }

void InitSettingsMenu(int &posX, int &posY, Menu &settingsMenu) {
  //MenuButton *curveButton =
  //    new MenuButton(&fontMenu, texturePaletteIconId, "", posX, posY += menuItemSize,
  //                   OnClickCurveScreen, nullptr, nullptr);

  MenuButton *screenModeButton = new MenuButton(&fontMenu, threedeeIconId, "", posX,
                                                posY += menuItemSize, OnClickScreenMode,
                                                OnClickScreenMode, OnClickScreenMode);

  MenuButton *paletteButton = new MenuButton(&fontMenu, texturePaletteIconId, "", posX,
                                             posY += menuItemSize + 5, OnClickPrefabColorRight,
                                             OnClickPrefabColorLeft, OnClickPrefabColorRight);

  rButton = new MenuButton(&fontMenu, texturePaletteIconId, "", posX,
                           posY += menuItemSize, nullptr, OnClickRLeft, OnClickRRight);
  gButton = new MenuButton(&fontMenu, texturePaletteIconId, "", posX,
                           posY += menuItemSize, nullptr, OnClickGLeft, OnClickGRight);
  bButton = new MenuButton(&fontMenu, texturePaletteIconId, "", posX,
                           posY += menuItemSize, nullptr, OnClickBLeft, OnClickBRight);

  //settingsMenu.MenuItems.push_back(curveButton);
  settingsMenu.MenuItems.push_back(screenModeButton);
  settingsMenu.MenuItems.push_back(paletteButton);
  settingsMenu.MenuItems.push_back(rButton);
  settingsMenu.MenuItems.push_back(gButton);
  settingsMenu.MenuItems.push_back(bButton);

  SetThreeDeeMode(screenModeButton, useThreeDeeMode);
  ChangePalette(paletteButton, 0);
}

void OnClickRom(Rom *rom) {
  LOG("LOAD ROM");
  LoadGame(rom);
  ResetMenuState();
}

void InitRomSelectionMenu(int posX, int posY, Menu &romSelectionMenu) {
  // rom list
  romList = new MenuList<Rom>(&fontList, OnClickRom, romFileList, 10, HEADER_HEIGHT + 10,
                              MENU_WIDTH - 20, (MENU_HEIGHT - HEADER_HEIGHT - BOTTOM_HEIGHT - 20));
  romList->CurrentSelection = romSelection;
  romSelectionMenu.MenuItems.push_back(romList);
}

void SaveEmulatorSettings(std::ofstream *saveFile) {
  saveFile->write(reinterpret_cast<const char *>(&romList->CurrentSelection), sizeof(int));
  saveFile->write(reinterpret_cast<const char *>(&color[0]), sizeof(float));
  saveFile->write(reinterpret_cast<const char *>(&color[1]), sizeof(float));
  saveFile->write(reinterpret_cast<const char *>(&color[2]), sizeof(float));
  saveFile->write(reinterpret_cast<const char *>(&selectedPredefColor), sizeof(int));
  saveFile->write(reinterpret_cast<const char *>(&useThreeDeeMode), sizeof(bool));

  // save button mapping
  for (int i = 0; i < buttonCount; ++i) {
    saveFile->write(reinterpret_cast<const char *>(&button_mapping_index[i]), sizeof(int));
  }
}

void LoadEmulatorSettings(std::ifstream *readFile) {
  readFile->read((char *) &romSelection, sizeof(int));
  readFile->read((char *) &color[0], sizeof(float));
  readFile->read((char *) &color[1], sizeof(float));
  readFile->read((char *) &color[2], sizeof(float));
  readFile->read((char *) &selectedPredefColor, sizeof(int));
  readFile->read((char *) &useThreeDeeMode, sizeof(bool));

  // load button mapping
  for (int i = 0; i < buttonCount; ++i) {
    readFile->read((char *) &button_mapping_index[i], sizeof(int));
    button_mapping[i] = 1u << button_mapping_index[i];
  }
}

void AddRom(std::string strFullPath, std::string strFilename) {
  size_t lastIndex = strFilename.find_last_of(".");
  std::string listName = strFilename.substr(0, lastIndex);
  size_t lastIndexSave = (strFullPath).find_last_of(".");
  std::string listNameSave = strFullPath.substr(0, lastIndexSave);

  Rom newRom;
  newRom.RomName = listName;
  newRom.FullPath = strFullPath;
  newRom.FullPathNorm = listNameSave;
  newRom.SavePath = listNameSave + ".srm";

  romFileList->push_back(newRom);

  LOG("found rom: %s %s %s", newRom.RomName.c_str(), newRom.FullPath.c_str(),
      newRom.SavePath.c_str());
}

void ResetGame() {
  VRVB::Reset();
}

void SaveRam() {
  if (CurrentRom != nullptr && VRVB::save_ram_size() > 0) {
    LOG("save ram %i", (int) VRVB::save_ram_size());
    std::ofstream outfile(CurrentRom->SavePath, std::ios::trunc | std::ios::binary);
    outfile.write((const char *) VRVB::save_ram(), VRVB::save_ram_size());
    outfile.close();
    LOG("finished writing ram file");
  }
}

void LoadRam() {
  std::ifstream file(CurrentRom->SavePath, std::ios::in | std::ios::binary | std::ios::ate);
  if (file.is_open()) {
    long romBufferSize = file.tellg();
    char *memblock = new char[romBufferSize];
    file.seekg(0, std::ios::beg);
    file.read(memblock, romBufferSize);
    file.close();
    LOG("loaded ram %ld", romBufferSize);

    LOG("ram size %i", (int) VRVB::save_ram_size());

    if (romBufferSize != (int) VRVB::save_ram_size()) {
      LOG("ERROR loaded ram size is wrong");
    } else {
      memcpy(VRVB::save_ram(), memblock, VRVB::save_ram_size());
      LOG("finished loading ram");
    }

    delete[] memblock;
  } else {
    LOG("could not load ram file: %s", CurrentRom->SavePath.c_str());
  }
}

void SaveState(int slot) {
  // get the size of the savestate
  size_t size = VRVB::retro_serialize_size();

  if (size > 0) {
    std::string savePath = stateFolderPath + CurrentRom->RomName + ".state";
    if (saveSlot > 0) savePath += to_string(saveSlot);

    LOG("save slot");
    void *data = new uint8_t[size];
    VRVB::retro_serialize(data, size);

    LOG("save slot to %s", savePath.c_str());
    std::ofstream outfile(savePath, std::ios::trunc | std::ios::binary);
    outfile.write((const char *) data, size);
    outfile.close();
    LOG("finished writing slot to file");
  }

  LOG("copy image");
  memcpy(currentGame->saveStates[saveSlot].saveImage, screenData,
         sizeof(uint8_t) * VIDEO_WIDTH * VIDEO_HEIGHT);
  LOG("update image");
  UpdateStateImage(saveSlot);
  // save image for the slot
  SaveStateImage(saveSlot);
  currentGame->saveStates[saveSlot].hasImage = true;
  currentGame->saveStates[saveSlot].hasState = true;
}

void LoadState(int slot) {
  std::string savePath = stateFolderPath + CurrentRom->RomName + ".state";
  if (slot > 0) savePath += to_string(slot);

  std::ifstream file(savePath, std::ios::in | std::ios::binary | std::ios::ate);
  if (file.is_open()) {
    long size = file.tellg();
    char *data = new char[size];

    file.seekg(0, std::ios::beg);
    file.read(data, size);
    file.close();
    LOG("loaded slot has size: %ld", size);

    VRVB::retro_unserialize(data, size);

    delete[] data;
  } else {
    LOG("could not load ram file: %s", CurrentRom->SavePath.c_str());
  }
}

void ChangeButtonMapping(int buttonIndex, int dir) {}

void UpdateButtonMapping() {}

void UpdateInput() {}

void Update(const ovrFrameInput &vrFrame, unsigned int lastButtonState) {
  // methode will only get called "emulationSpeed" times a second
  frameCounter += vrFrame.DeltaSeconds;
  if (frameCounter < 1 / emulationSpeed) {
    return;
  }
  frameCounter -= 1 / emulationSpeed;

  VRVB::input_buf[0] = 0;
  VRVB::input_buf[0] |= (vrFrame.Input.buttonState & button_mapping[0]) ? 1 : 0;
  VRVB::input_buf[0] |= (vrFrame.Input.buttonState & button_mapping[1]) ? 2 : 0;
  VRVB::input_buf[0] |= (vrFrame.Input.buttonState & button_mapping[2]) ? 4 : 0;
  VRVB::input_buf[0] |= (vrFrame.Input.buttonState & button_mapping[3]) ? 8 : 0;
  VRVB::input_buf[0] |= (vrFrame.Input.buttonState & button_mapping[4]) ? 16 : 0;
  VRVB::input_buf[0] |= (vrFrame.Input.buttonState & button_mapping[5]) ? 32 : 0;
  VRVB::input_buf[0] |= (vrFrame.Input.buttonState & button_mapping[6]) ? 64 : 0;
  VRVB::input_buf[0] |= (vrFrame.Input.buttonState & button_mapping[7]) ? 128 : 0;
  VRVB::input_buf[0] |= (vrFrame.Input.buttonState & button_mapping[8]) ? 256 : 0;
  VRVB::input_buf[0] |= (vrFrame.Input.buttonState & button_mapping[9]) ? 512 : 0;
  VRVB::input_buf[0] |= (vrFrame.Input.buttonState & button_mapping[10]) ? 1024 : 0;
  VRVB::input_buf[0] |= (vrFrame.Input.buttonState & button_mapping[11]) ? 2048 : 0;
  VRVB::input_buf[0] |= (vrFrame.Input.buttonState & button_mapping[12]) ? 4096 : 0;
  VRVB::input_buf[0] |= (vrFrame.Input.buttonState & button_mapping[13]) ? 8192 : 0;

  VRVB::Run();
}

// Aspect is width / height
Matrix4f BoundsScreenMatrix(const Bounds3f &bounds, const float movieAspect) {
  const Vector3f size = bounds.b[1] - bounds.b[0];
  const Vector3f center = bounds.b[0] + size * 0.5f;

  const float screenHeight = size.y;
  const float screenWidth = OVR::Alg::Max(size.x, size.z);

  float widthScale;
  float heightScale;

  float aspect = (movieAspect == 0.0f) ? 1.0f : movieAspect;

  if (screenWidth / screenHeight > aspect) {    // screen is wider than movie, clamp size to height
    heightScale = screenHeight * 0.5f;
    widthScale = heightScale * aspect;
  } else {    // screen is taller than movie, clamp size to width
    widthScale = screenWidth * 0.5f;
    heightScale = widthScale / aspect;
  }

  const float rotateAngle = (size.x > size.z) ? 0.0f : MATH_FLOAT_PI * 0.5f;

  return Matrix4f::Translation(center) *
      Matrix4f::RotationY(rotateAngle) *
      Matrix4f::Scaling(widthScale, heightScale, 1.0f);
}

Matrix4f ScreenMatrix() {
  return BoundsScreenMatrix(SceneScreenBounds,
                            VIDEO_WIDTH
                                / (float) VIDEO_HEIGHT); // : ( (float)CurrentMovieWidth / CurrentMovieHeight )
}

void DrawScreenLayer(ovrFrameResult &res, const ovrFrameInput &vrFrame) {

  /*
       res.Layers[res.LayerCount].Cube = LayerBuilder::BuildCubeLayer(
        CylinderSwapChainCubeLeft,
        !menuOpen ? CylinderSwapChainCubeRight : CylinderSwapChainCubeLeft, &vrFrame.Tracking,
        followHead);
    res.Layers[res.LayerCount].Cube.Header.Flags |=
        VRAPI_FRAME_LAYER_FLAG_CHROMATIC_ABERRATION_CORRECTION;
    res.LayerCount++;
   */

  if (useCubeMap) {
    Matrix4f texMatrix[2];

    float imgHeight = 0.5f;// (VIDEO_HEIGHT * 2) / (float) (TextureHeight * 2);
    const Matrix4f stretchTop(
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, imgHeight, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f);
    const Matrix4f stretchBottom(
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, imgHeight, 1 - imgHeight, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f);

    texMatrix[0] = texMatrix[1] = Matrix4f::Identity();

    texMatrix[0] = stretchTop;
    texMatrix[1] = stretchBottom;

    ScreenTexMatrix[0] = texMatrix[0].Transposed();
    ScreenTexMatrix[1] = texMatrix[1].Transposed();

    ScreenTexMatrices.Update(2 * sizeof(Matrix4f), &ScreenTexMatrix[0]);

    ScreenSurfaceDef.graphicsCommand.GpuState.depthEnable = false;
    res.Surfaces.PushBack(ovrDrawSurface(ScreenMatrix(), &ScreenSurfaceDef));
  } else {
    // virtual screen layer
    res.Layers[res.LayerCount].Cylinder = LayerBuilder::BuildGameCylinderLayer3D(
        CylinderSwapChain, CylinderWidth, CylinderHeight, &vrFrame.Tracking, followHead,
        !menuOpen && useThreeDeeMode);
    res.Layers[res.LayerCount].Cylinder.Header.Flags |=
        VRAPI_FRAME_LAYER_FLAG_CHROMATIC_ABERRATION_CORRECTION;
    res.Layers[res.LayerCount].Cylinder.Header.Flags |=
        VRAPI_FRAME_LAYER_FLAG_INHIBIT_SRGB_FRAMEBUFFER;
    res.LayerCount++;
  }
}

}  // namespace VBEmulator
