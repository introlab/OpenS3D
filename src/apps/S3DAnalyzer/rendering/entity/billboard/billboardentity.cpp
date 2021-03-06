#include "billboardentity.h"

#include <QColor>

#include <cassert>

BillboardEntity::BillboardEntity(const QSize& imageSize) : m_imageSize(imageSize) {
  initializeOpenGLFunctions();
}

BillboardEntity::~BillboardEntity() {
  m_object.destroy();
  m_vertex.destroy();
  m_program.reset();
}

void BillboardEntity::init() {
  createShader();
  createAndBindBuffer();
  createVertexArrayObject();
  releaseAll();
}

void BillboardEntity::createShader() {
  m_program = std::make_unique<QOpenGLShaderProgram>();
  addShaders();
  m_program->link();
  m_program->bind();
}

void BillboardEntity::createAndBindBuffer() {
  m_vertex.create();
  m_vertex.bind();
  m_vertex.setUsagePattern(QOpenGLBuffer::StaticDraw);
}

void BillboardEntity::createVertexArrayObject() {
  m_object.create();
  m_object.bind();
  m_program->bind();
  m_program->enableAttributeArray(0);
  m_program->enableAttributeArray(1);
  setDefaultAttributeBuffers();
  setDefaultUniforms();
}

void BillboardEntity::releaseAll() {
  m_object.release();
  m_vertex.release();
  m_program->release();
}

void BillboardEntity::setHorizontalShift(float shift) {
  m_program->bind();
  { m_program->setUniformValue("uHorizontalShift", shift); }
  m_program->release();
}

void BillboardEntity::setAspectRatio(float ratio) {
  m_program->bind();
  { m_program->setUniformValue("uAspectRatio", ratio); }
  m_program->release();
}

void BillboardEntity::setDisplayRange(float minX, float maxX, float minY, float maxY) {
  float w = maxX - minX;
  float h = maxY - minY;
  m_imageSize = QSize(static_cast<int>(w), static_cast<int>(h));
  float values[] = {2.0f / w, 0.0f, -1.0f, 0.0f, -2.0f / h, 1.0f, 0.0f, 0.0f, 1.0f};
  setPointToScreen(QMatrix3x3(values));
}

void BillboardEntity::clear() {
  m_vertices.clear();
}

void BillboardEntity::draw(QPaintDevice* /*paintDevice*/) {
  if (!m_vertices.empty()) {
    // Render using our shader
    m_program->bind();
    {
      m_object.bind();
      { glDrawArrays(GL_POINTS, 0, m_vertices.size()); }
      m_object.release();
    }
    m_program->release();
  }
}

void BillboardEntity::setDefaultUniforms() {
  m_program->bind();
  {
    m_program->setUniformValue("uPointSize", 10.0f);
    setDisplayRange(0, m_imageSize.width(), 0, m_imageSize.height());
    setHorizontalShift(0.0f);
    setAspectRatio(1.0f);
  }
  m_program->release();
}

void BillboardEntity::setPointToScreen(const QMatrix3x3 pointToScreen) {
  m_program->bind();
  { m_program->setUniformValue("uPointToScreen", pointToScreen); }
  m_program->release();
}
